/*
 * rhd_http.c -- HTTP I/O helpers for Raptor HTTP Daemon
 *
 * TLS-aware read/write, HTTP response formatting, MJPEG framing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/epoll.h>
#include <poll.h>

#include "rhd.h"

/* ── TLS-aware I/O wrappers ── */

ssize_t rhd_write(rhd_client_t *c, const void *buf, size_t len)
{
#ifdef RSS_HAS_TLS
	if (c->tls)
		return rss_tls_write(c->tls, buf, len);
#endif
	return write(c->fd, buf, len);
}

ssize_t rhd_read(rhd_client_t *c, void *buf, size_t len)
{
#ifdef RSS_HAS_TLS
	/* Lazy TLS handshake on first read */
	if (!c->tls && c->srv_tls) {
		c->tls = rss_tls_accept(c->srv_tls, c->fd, 5000);
		if (!c->tls)
			return -1;
	}
	if (c->tls)
		return rss_tls_read(c->tls, buf, len);
#endif
	return read(c->fd, buf, len);
}

/* ── HTTP response helpers ── */

/* Small synchronous send for error/status responses (< 1KB, always fits in socket buffer) */
void http_send(rhd_client_t *c, const char *status, const char *content_type, const void *body,
	       int body_len)
{
	char header[512];
	int hlen = snprintf(header, sizeof(header),
			    "HTTP/1.1 %s\r\n"
			    "Content-Type: %s\r\n"
			    "Content-Length: %d\r\n"
			    "Connection: close\r\n"
			    "Access-Control-Allow-Origin: *\r\n"
			    "\r\n",
			    status, content_type, body_len);
	rhd_write(c, header, hlen);
	if (body && body_len > 0)
		rhd_write(c, body, body_len);
}

/* Plain fd version for pre-TLS error responses (e.g., max clients rejection) */
/*
 * Send @len bytes from @buf to @fd, looping over partial writes and
 * retrying on EINTR. The only errors worth flagging here are ECONNRESET /
 * EPIPE (client dropped) and ENOSPC (disk-backed socket, shouldn't
 * happen) — both are non-fatal from the daemon's perspective. Log at
 * DEBUG level so operators can see repeated client resets under load
 * without spamming the log on the common happy path.
 */
static void http_write_all(int fd, const void *buf, size_t len, const char *what)
{
	const char *p = buf;
	size_t remaining = len;
	while (remaining > 0) {
		ssize_t n = write(fd, p, remaining);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			RSS_DEBUG("http_send_fd: %s write failed after %zu/%zu bytes: %s",
				  what, len - remaining, len, strerror(errno));
			return;
		}
		p += n;
		remaining -= (size_t)n;
	}
}

void http_send_fd(int fd, const char *status, const char *content_type, const void *body,
		  int body_len)
{
	char header[512];
	int hlen = snprintf(header, sizeof(header),
			    "HTTP/1.1 %s\r\n"
			    "Content-Type: %s\r\n"
			    "Content-Length: %d\r\n"
			    "Connection: close\r\n"
			    "\r\n",
			    status, content_type, body_len);
	if (hlen < 0 || hlen >= (int)sizeof(header)) {
		/* snprintf truncation — status/content_type came from a
		 * caller with an oversized string. Bail rather than send a
		 * partial header that the client would reject anyway. */
		RSS_DEBUG("http_send_fd: header truncated (len=%d)", hlen);
		return;
	}
	http_write_all(fd, header, (size_t)hlen, "header");
	if (body && body_len > 0)
		http_write_all(fd, body, (size_t)body_len, "body");
}

/* Queue a large response for non-blocking send via epoll.
 * Builds header + body into a single heap buffer on the client. */
int http_send_async(rhd_client_t *c, int epoll_fd, const char *content_type, const void *body,
		    uint32_t body_len)
{
	char header[512];
	int hlen = snprintf(header, sizeof(header),
			    "HTTP/1.1 200 OK\r\n"
			    "Content-Type: %s\r\n"
			    "Content-Length: %u\r\n"
			    "Connection: close\r\n"
			    "Access-Control-Allow-Origin: *\r\n"
			    "\r\n",
			    content_type, body_len);

	if (hlen < 0)
		return -1;
	c->send_buf = malloc((size_t)hlen + body_len);
	if (!c->send_buf)
		return -1;
	memcpy(c->send_buf, header, (size_t)hlen);
	memcpy(c->send_buf + hlen, body, body_len);
	c->send_len = (uint32_t)((size_t)hlen + body_len);
	c->send_off = 0;

	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	c->send_start = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

	/* Switch to EPOLLOUT to drive the send */
	struct epoll_event ev = {.events = EPOLLOUT | EPOLLHUP | EPOLLERR, .data.fd = c->fd};
	if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, c->fd, &ev) < 0) {
		RSS_ERROR("epoll_ctl mod client fd: %s", strerror(errno));
		free(c->send_buf);
		c->send_buf = NULL;
		c->send_len = 0;
		return -1;
	}
	return 0;
}

void http_error(rhd_client_t *c, const char *status, const char *msg)
{
	http_send(c, status, "text/plain", msg, (int)strlen(msg));
}

void http_401(rhd_client_t *c)
{
	const char *body = "Unauthorized";
	char header[512];
	int hlen = snprintf(header, sizeof(header),
			    "HTTP/1.1 401 Unauthorized\r\n"
			    "WWW-Authenticate: Basic realm=\"Raptor\"\r\n"
			    "Content-Type: text/plain\r\n"
			    "Content-Length: %d\r\n"
			    "Connection: close\r\n"
			    "\r\n",
			    (int)strlen(body));
	rhd_write(c, header, hlen);
	rhd_write(c, body, strlen(body));
}

/* Auth wrapper using shared rss_http helper */
bool http_check_auth(const rhd_server_t *srv, const char *request)
{
	return rss_http_check_basic_auth(request, srv->auth_user, srv->auth_pass);
}

void http_send_mjpeg_header(rhd_client_t *c)
{
	char header[256];
	int hlen = snprintf(header, sizeof(header),
			    "HTTP/1.1 200 OK\r\n"
			    "Content-Type: multipart/x-mixed-replace;boundary=" RHD_MJPEG_BOUNDARY
			    "\r\n"
			    "Cache-Control: no-cache\r\n"
			    "Access-Control-Allow-Origin: *\r\n"
			    "\r\n");
	rhd_write(c, header, hlen);
}

/*
 * Write all bytes to a non-blocking fd, retrying on EAGAIN with poll().
 * Returns 0 on success, -1 if the client is dead or stalled beyond timeout.
 */
int nb_write_all(rhd_client_t *c, const void *buf, size_t len)
{
	const uint8_t *p = buf;
	size_t remaining = len;
	int retries = 0;

	while (remaining > 0) {
		ssize_t n = rhd_write(c, p, remaining);
		if (n > 0) {
			p += n;
			remaining -= (size_t)n;
			retries = 0;
		} else if (n < 0) {
			if ((errno == EAGAIN || errno == EWOULDBLOCK) && retries < 50) {
				/* Back off: poll for write-ready, 100ms max per retry */
				struct pollfd pfd = {.fd = c->fd, .events = POLLOUT};
				poll(&pfd, 1, 100);
				retries++;
			} else {
				return -1; /* dead or stalled too long */
			}
		}
	}
	return 0;
}

int http_send_mjpeg_frame(rhd_client_t *c, const uint8_t *data, uint32_t len)
{
	char part_hdr[128];
	int hlen = snprintf(part_hdr, sizeof(part_hdr),
			    "--" RHD_MJPEG_BOUNDARY "\r\n"
			    "Content-Type: image/jpeg\r\n"
			    "Content-Length: %u\r\n"
			    "\r\n",
			    len);

	/* Combine header + JPEG + CRLF into a single write to avoid
	 * TLS record fragmentation that causes render flicker in browsers. */
	if (hlen < 0)
		return -1;
	size_t total = (size_t)hlen + len + 2;
	uint8_t *frame = malloc(total);
	if (!frame)
		return -1;
	memcpy(frame, part_hdr, (size_t)hlen);
	memcpy(frame + hlen, data, len);
	frame[hlen + len] = '\r';
	frame[hlen + len + 1] = '\n';
	int ret = nb_write_all(c, frame, total);
	free(frame);
	return ret;
}

/*
 * rwd_signaling.c -- WHIP HTTP signaling endpoint
 *
 * Minimal HTTP handler for WebRTC-HTTP Ingestion Protocol:
 *   GET  /webrtc  → serve player HTML page
 *   POST /whip    → SDP offer/answer exchange
 *   DELETE /whip/{session} → teardown session
 *
 * Also handles CORS preflight (OPTIONS) for cross-origin requests.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>

#ifdef RSS_HAS_TLS
#include <rss_tls.h>
#endif
#include <netinet/in.h>
#include <arpa/inet.h>

#include "rwd.h"
#include <rss_http.h>

/* WebRTC player page — loaded from file on first request, cached.
 * Edit the file on device and restart RWD to pick up changes. */
#define WEBRTC_HTML_PATH "/usr/share/raptor/webrtc.html"
static char *webrtc_html;
static int webrtc_html_len;

/* ── HTTP I/O helpers (plain or TLS) ── */

#ifdef RSS_HAS_TLS
static __thread rss_tls_conn_t *tls_conn; /* set per-request */
#endif

static ssize_t http_write(int fd, const void *buf, size_t len)
{
#ifdef RSS_HAS_TLS
	if (tls_conn)
		return rss_tls_write(tls_conn, buf, len);
#endif
	return write(fd, buf, len);
}

static ssize_t http_read(int fd, void *buf, size_t len)
{
#ifdef RSS_HAS_TLS
	if (tls_conn)
		return rss_tls_read(tls_conn, buf, len);
#endif
	return read(fd, buf, len);
}

static void http_send(int fd, const char *status, const char *content_type, const char *body,
		      size_t body_len, const char *extra_headers)
{
	char hdr[1024];
	int hdr_len = snprintf(hdr, sizeof(hdr),
			       "HTTP/1.1 %s\r\n"
			       "Content-Type: %s\r\n"
			       "Content-Length: %zu\r\n"
			       "Access-Control-Allow-Origin: *\r\n"
			       "Access-Control-Allow-Methods: POST, DELETE, OPTIONS\r\n"
			       "Access-Control-Allow-Headers: Content-Type\r\n"
			       "%s"
			       "Connection: close\r\n"
			       "\r\n",
			       status, content_type, body_len, extra_headers ? extra_headers : "");

	if (hdr_len < 0)
		hdr_len = 0;
	if ((size_t)hdr_len > sizeof(hdr))
		hdr_len = sizeof(hdr);

	(void)!http_write(fd, hdr, hdr_len);
	if (body && body_len > 0)
		(void)!http_write(fd, body, body_len);
}

static void http_error(int fd, const char *status)
{
	http_send(fd, status, "text/plain", status, strlen(status), NULL);
}

static void http_close(int fd)
{
#ifdef RSS_HAS_TLS
	rss_tls_close(tls_conn);
	tls_conn = NULL;
#endif
	close(fd);
}

/* ── Generate session ID ── */

static void generate_session_id(char *out, size_t out_size)
{
	uint8_t bytes[RWD_SESSION_ID_LEN];
	if (rwd_random_bytes(bytes, sizeof(bytes)) != 0) {
		uint64_t ts = rss_timestamp_us();
		memcpy(bytes, &ts, sizeof(ts) < sizeof(bytes) ? sizeof(ts) : sizeof(bytes));
	}
	for (int i = 0; i < RWD_SESSION_ID_LEN && (size_t)(i * 2 + 2) < out_size; i++)
		snprintf(out + i * 2, 3, "%02x", bytes[i]);
}

/* ── Create client from SDP offer (shared by WHIP and WebTorrent) ── */

rwd_client_t *rwd_client_from_offer(rwd_server_t *srv, const char *sdp, int stream_idx,
				    char *sdp_answer, size_t sdp_answer_size)
{
	if (stream_idx < 0 || stream_idx >= RWD_STREAM_COUNT)
		stream_idx = 0;

	rwd_sdp_offer_t offer;
	if (rwd_sdp_parse_offer(sdp, &offer) != 0)
		return NULL;

	pthread_mutex_lock(&srv->clients_lock);
	if (srv->client_count >= srv->max_clients) {
		pthread_mutex_unlock(&srv->clients_lock);
		return NULL;
	}

	rwd_client_t *c = calloc(1, sizeof(*c));
	if (!c) {
		pthread_mutex_unlock(&srv->clients_lock);
		return NULL;
	}

	c->server = srv;
	c->active = true;
	c->stream_idx = stream_idx;
	c->created_at = rss_timestamp_us();
	memcpy(&c->offer, &offer, sizeof(offer));
	rss_strlcpy(c->remote_ufrag, offer.ice_ufrag, sizeof(c->remote_ufrag));
	rss_strlcpy(c->remote_pwd, offer.ice_pwd, sizeof(c->remote_pwd));

	rwd_generate_ice_credentials(c->local_ufrag, sizeof(c->local_ufrag), c->local_pwd,
				     sizeof(c->local_pwd));
	uint8_t ssrc_bytes[8];
	if (rwd_random_bytes(ssrc_bytes, sizeof(ssrc_bytes)) != 0) {
		static uint32_t ssrc_counter; /* safe: always called under clients_lock */
		uint64_t ts = rss_timestamp_us();
		memcpy(ssrc_bytes, &ts, sizeof(ssrc_bytes));
		ssrc_bytes[0] ^= (uint8_t)(ssrc_counter);
		ssrc_bytes[4] ^= (uint8_t)(ssrc_counter >> 8);
		ssrc_counter++;
	}
	memcpy(&c->video_ssrc, ssrc_bytes, 4);
	memcpy(&c->audio_ssrc, ssrc_bytes + 4, 4);
	if (c->video_ssrc == c->audio_ssrc)
		c->audio_ssrc ^= 0x01;

	generate_session_id(c->session_id, sizeof(c->session_id));

	if (rwd_dtls_client_init(c, srv->dtls) != 0) {
		free(c);
		pthread_mutex_unlock(&srv->clients_lock);
		return NULL;
	}

	/* Re-detect local IP per session (handles late DHCP / IP change) */
	if (!srv->local_ip_configured) {
		char ip[64];
		if (rwd_get_local_ip(ip, sizeof(ip)) == 0)
			rss_strlcpy(srv->local_ip, ip, sizeof(srv->local_ip));
	}

	int sdp_len = rwd_sdp_generate_answer(c, srv, sdp_answer, sdp_answer_size);
	if (sdp_len < 0) {
		rwd_dtls_client_free(c);
		free(c);
		pthread_mutex_unlock(&srv->clients_lock);
		return NULL;
	}

	for (int i = 0; i < RWD_MAX_CLIENTS; i++) {
		if (!srv->clients[i]) {
			srv->clients[i] = c;
			srv->client_count++;
			break;
		}
	}
	pthread_mutex_unlock(&srv->clients_lock);

	RSS_DEBUG("client created: session %s (video_pt=%d audio_pt=%d)", c->session_id,
		  c->offer.video_pt, c->offer.audio_pt);
	return c;
}

/* ── WHIP POST handler: SDP offer → answer ── */

static void handle_whip_post(rwd_server_t *srv, int fd, const char *body, size_t body_len,
			     const struct sockaddr_storage *local_addr, int stream_idx)
{
	(void)body_len;
	(void)local_addr;

	char sdp_answer[RWD_SDP_BUF_SIZE];
	rwd_client_t *c =
		rwd_client_from_offer(srv, body, stream_idx, sdp_answer, sizeof(sdp_answer));
	if (!c) {
		http_error(fd, "400 Bad Request");
		return;
	}

	char location[128];
	snprintf(location, sizeof(location), "Location: /whip/%s\r\n", c->session_id);
	http_send(fd, "201 Created", "application/sdp", sdp_answer, strlen(sdp_answer), location);

	RSS_INFO("WHIP: session %s created", c->session_id);
}

/* ── WHIP DELETE handler: teardown session ── */

static void handle_whip_delete(rwd_server_t *srv, int fd, const char *session_id)
{
	pthread_mutex_lock(&srv->clients_lock);
	for (int i = 0; i < RWD_MAX_CLIENTS; i++) {
		if (srv->clients[i] && strcmp(srv->clients[i]->session_id, session_id) == 0) {
			rwd_client_t *c = srv->clients[i];
			rwd_media_teardown(c);
			rwd_dtls_client_free(c);
			free(c);
			srv->clients[i] = NULL;
			srv->client_count--;
			RSS_INFO("WHIP: session %s deleted", session_id);
			break;
		}
	}
	pthread_mutex_unlock(&srv->clients_lock);

	http_send(fd, "200 OK", "text/plain", "OK", 2, NULL);
}

void rwd_signaling_cleanup(void)
{
	free(webrtc_html);
	webrtc_html = NULL;
}

/* ── Main HTTP request handler ── */

void rwd_signaling_handle(rwd_server_t *srv, int client_fd,
			  const struct sockaddr_storage *local_addr)
{
#ifdef RSS_HAS_TLS
	/* TLS handshake if HTTPS context available */
	tls_conn = NULL;
	if (srv->tls) {
		tls_conn = rss_tls_accept(srv->tls, client_fd, 5000);
		if (!tls_conn) {
			http_close(client_fd);
			return;
		}
	}
#endif

	/* Read HTTP request — may need multiple reads over TLS since
	 * headers and body can arrive in separate TLS records. */
	char buf[RWD_HTTP_BUF_SIZE];
	ssize_t n = 0;
	for (int i = 0; i < 50 && n < (ssize_t)(sizeof(buf) - 1); i++) {
		ssize_t r = http_read(client_fd, buf + n, sizeof(buf) - 1 - n);
		if (r > 0) {
			n += r;
			buf[n] = '\0';
			const char *hdr_end = strstr(buf, "\r\n\r\n");
			if (hdr_end) {
				const char *cl = strcasestr(buf, "Content-Length:");
				if (!cl)
					break; /* no body expected */
				size_t clen = strtoul(cl + 15, NULL, 10);
				if ((size_t)n >= (size_t)(hdr_end + 4 - buf) + clen)
					break; /* full body received */
			}
		} else if (r == 0) {
			break;
		} else if (i == 0) {
			/* First read failed — poll briefly for plain HTTP */
			struct pollfd pfd = {.fd = client_fd, .events = POLLIN};
			if (poll(&pfd, 1, 100) <= 0)
				break;
		} else {
			usleep(5000);
		}
	}
	if (n <= 0) {
		http_close(client_fd);
		return;
	}
	buf[n] = '\0';

	/* Parse request line */
	char method[16], path[256];
	if (sscanf(buf, "%15s %255s", method, path) != 2) {
		http_error(client_fd, "400 Bad Request");
		http_close(client_fd);
		return;
	}

	/* Reject oversized requests */
	if (n >= (ssize_t)(sizeof(buf) - 1)) {
		http_error(client_fd, "414 URI Too Long");
		http_close(client_fd);
		return;
	}

	/* CORS preflight */
	if (strcmp(method, "OPTIONS") == 0) {
		http_send(client_fd, "204 No Content", "text/plain", NULL, 0, NULL);
		http_close(client_fd);
		return;
	}

	/* Basic auth check (if configured) */
	if (srv->auth_user[0] && srv->auth_pass[0]) {
		if (!rss_http_check_basic_auth(buf, srv->auth_user, srv->auth_pass)) {
			http_send(client_fd, "401 Unauthorized", "text/plain",
				  "Unauthorized", 12,
				  "WWW-Authenticate: Basic realm=\"Raptor WebRTC\"\r\n");
			http_close(client_fd);
			return;
		}
	}

	/* GET /webrtc — serve player page */
	if (strcmp(method, "GET") == 0 && strcmp(path, "/webrtc") == 0) {
		if (!webrtc_html) {
			webrtc_html = rss_read_file(WEBRTC_HTML_PATH, &webrtc_html_len);
			if (webrtc_html)
				RSS_DEBUG("loaded %s (%d bytes)", WEBRTC_HTML_PATH, webrtc_html_len);
			else
				RSS_WARN("%s not found", WEBRTC_HTML_PATH);
		}
		if (webrtc_html)
			http_send(client_fd, "200 OK", "text/html; charset=utf-8",
				  webrtc_html, (size_t)webrtc_html_len,
				  "Cache-Control: no-cache\r\n");
		else
			http_send(client_fd, "404 Not Found", "text/plain",
				  "player not installed", 20, NULL);
		http_close(client_fd);
		return;
	}

	/* POST /whip — SDP offer/answer */
	if (strcmp(method, "POST") == 0 && strncmp(path, "/whip", 5) == 0) {
		/* Parse ?stream=N from query string */
		int stream_idx = 0;
		const char *qs = strchr(path, '?');
		if (qs) {
			const char *sp = strstr(qs, "stream=");
			if (sp)
				stream_idx = (sp[7] == '1') ? 1 : 0;
		}
		/* Parse Content-Length */
		size_t content_length = 0;
		const char *cl = strcasestr(buf, "Content-Length:");
		if (cl) {
			char *end;
			long cl_val = strtol(cl + 15, &end, 10);
			if (cl_val > 0 && cl_val < (long)sizeof(buf) && end != cl + 15)
				content_length = (size_t)cl_val;
		}

		/* Find body (after \r\n\r\n) */
		const char *body = strstr(buf, "\r\n\r\n");
		if (!body) {
			RSS_WARN("WHIP: no body separator in request");
			http_error(client_fd, "400 Bad Request");
			http_close(client_fd);
			return;
		}
		body += 4;
		size_t body_len = n - (body - buf);

		/* Read remaining body if we didn't get it all.
		 * Poll with a tight timeout — legitimate clients deliver
		 * the body within milliseconds of the header. */
		size_t need = content_length > 0 ? content_length : 0;
		while (body_len < need && (size_t)n < sizeof(buf) - 1) {
			struct pollfd pfd = {.fd = client_fd, .events = POLLIN};
			if (poll(&pfd, 1, 500) <= 0)
				break;
			ssize_t more = http_read(client_fd, buf + n, sizeof(buf) - 1 - n);
			if (more <= 0)
				break;
			n += more;
			buf[n] = '\0';
			body_len = n - (body - buf);
		}

		handle_whip_post(srv, client_fd, body, body_len, local_addr, stream_idx);
		http_close(client_fd);
		return;
	}

	/* DELETE /whip/{session} — teardown */
	if (strcmp(method, "DELETE") == 0 && strncmp(path, "/whip/", 6) == 0) {
		const char *session_id = path + 6;
		if (strlen(session_id) > 0) {
			handle_whip_delete(srv, client_fd, session_id);
			http_close(client_fd);
			return;
		}
	}

	http_error(client_fd, "404 Not Found");
	http_close(client_fd);
}

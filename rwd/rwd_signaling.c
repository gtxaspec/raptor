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
#include <pthread.h>
#include <stdatomic.h>
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
static pthread_mutex_t webrtc_html_lock = PTHREAD_MUTEX_INITIALIZER;

/* Signaling runs on worker threads, never on the media loop: a client that
 * connects and stalls must not stop STUN, DTLS and RTP for every viewer.
 * At most this many requests at once, each bounded by one deadline. */
#define RWD_SIGNALING_WORKERS	  4
#define RWD_SIGNALING_DEADLINE_MS 5000

static atomic_int signaling_workers;
static __thread int64_t req_deadline_us;

static int req_wait_ms(void)
{
	int64_t left = (req_deadline_us - rss_timestamp_us()) / 1000;
	return left > 0 ? (int)left : 0;
}

/* ── HTTP I/O helpers (plain or TLS) ── */

#ifdef RSS_HAS_TLS
static __thread rss_tls_conn_t *tls_conn; /* set per-request */
#endif

/* The socket is non-blocking: both helpers wait, but only until the
 * request's deadline. */
static ssize_t http_write(int fd, const void *buf, size_t len)
{
#ifdef RSS_HAS_TLS
	if (tls_conn)
		return rss_tls_write(tls_conn, buf, len);
#endif
	const char *p = buf;
	size_t done = 0;
	while (done < len) {
		ssize_t w = write(fd, p + done, len - done);
		if (w > 0) {
			done += (size_t)w;
			continue;
		}
		if (w < 0 && errno == EINTR)
			continue;
		struct pollfd pfd = {.fd = fd, .events = POLLOUT};
		if (w < 0 && errno == EAGAIN && poll(&pfd, 1, req_wait_ms()) > 0)
			continue;
		break;
	}
	return done > 0 ? (ssize_t)done : -1;
}

static ssize_t http_read(int fd, void *buf, size_t len)
{
#ifdef RSS_HAS_TLS
	if (tls_conn)
		return rss_tls_read(tls_conn, buf, len);
#endif
	for (;;) {
		ssize_t r = read(fd, buf, len);
		if (r >= 0)
			return r;
		if (errno == EINTR)
			continue;
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			return -1;
		struct pollfd pfd = {.fd = fd, .events = POLLIN};
		if (poll(&pfd, 1, req_wait_ms()) <= 0)
			return -1;
	}
}

/* Cross-origin access is offered only when credentials gate the endpoint:
 * without them a web page in any LAN browser could open a session. Set per
 * request by the worker serving it. */
static __thread bool cors_enabled;

static void http_send(int fd, const char *status, const char *content_type, const char *body,
		      size_t body_len, const char *extra_headers)
{
	char hdr[1024];
	int hdr_len =
		snprintf(hdr, sizeof(hdr),
			 "HTTP/1.1 %s\r\n"
			 "Content-Type: %s\r\n"
			 "Content-Length: %zu\r\n"
			 "%s%s"
			 "Connection: close\r\n"
			 "\r\n",
			 status, content_type, body_len,
			 cors_enabled ? "Access-Control-Allow-Origin: *\r\n"
					"Access-Control-Allow-Methods: POST, DELETE, OPTIONS\r\n"
					"Access-Control-Allow-Headers: Content-Type, "
					"Authorization\r\n"
				      : "",
			 extra_headers ? extra_headers : "");

	if (hdr_len < 0)
		hdr_len = 0;
	if ((size_t)hdr_len > sizeof(hdr))
		hdr_len = sizeof(hdr);

	(void)!http_write(fd, hdr, hdr_len);
	if (body && body_len > 0)
		(void)!http_write(fd, body, body_len);
}

static void http_error(int fd, const char *status, const char *reason)
{
	char body[160];

	if (reason && *reason)
		snprintf(body, sizeof(body), "%s: %s", status, reason);
	else
		snprintf(body, sizeof(body), "%s", status);
	http_send(fd, status, "text/plain", body, strlen(body), NULL);
}

/* Human-readable cause for a rejected SDP offer, safe for an HTTP body. */
static const char *sdp_reject_reason(rwd_sdp_reject_t r)
{
	switch (r) {
	case RWD_SDP_ERR_ICE_CREDENTIALS:
		return "SDP offer missing ICE credentials (ice-ufrag/ice-pwd)";
	case RWD_SDP_ERR_NO_FINGERPRINT:
		return "SDP offer missing DTLS fingerprint";
	case RWD_SDP_ERR_NO_H264:
		return "no H264 video codec in offer; browser may lack an H264 encoder "
		       "(e.g. Firefox without the OpenH264 plugin)";
	case RWD_SDP_ERR_SYNTAX:
		return "request body is not a parseable SDP offer";
	default:
		return NULL;
	}
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
	memset(bytes, 0, sizeof(bytes));
	if (rwd_random_bytes(bytes, sizeof(bytes)) != 0) {
		/* Fallback: timestamp + zero padding (no uninitialized stack leak) */
		uint64_t ts = rss_timestamp_us();
		memcpy(bytes, &ts, sizeof(ts) < sizeof(bytes) ? sizeof(ts) : sizeof(bytes));
	}
	for (int i = 0; i < RWD_SESSION_ID_LEN && (size_t)(i * 2 + 2) < out_size; i++)
		snprintf(out + i * 2, 3, "%02x", bytes[i]);
}

/* ── Create client from SDP offer (shared by WHIP and WebTorrent) ── */

bool rwd_client_from_offer(rwd_server_t *srv, const char *sdp, int stream_idx, char *sdp_answer,
			   size_t sdp_answer_size, rwd_sdp_reject_t *reject,
			   char session_id_out[RWD_SESSION_ID_LEN * 2 + 1])
{
	if (stream_idx < 0 || stream_idx >= RWD_STREAM_COUNT)
		stream_idx = 0;

	/* Default reason covers later failures; the most common is having no
	 * local address for the answer. Parse failure overrides it below. */
	if (reject)
		*reject =
			(srv->local_ip[0] == '\0') ? RWD_SDP_ERR_NO_LOCAL_IP : RWD_SDP_ERR_UNKNOWN;
	rwd_sdp_offer_t offer;
	rwd_sdp_reject_t parsed = rwd_sdp_parse_offer_r(sdp, &offer);
	if (parsed != RWD_SDP_OK) {
		if (reject)
			*reject = parsed;
		return false;
	}

	pthread_mutex_lock(&srv->clients_lock);
	if (srv->client_count >= srv->max_clients) {
		/* Known gap: this lets any caller who may POST disconnect the
		 * oldest viewer. Refusing instead locks a viewer out of their
		 * own reload, because a closed tab's session lingers until
		 * consent expires; needs dead-session detection first. */
		int oldest_idx = -1;
		int64_t oldest_ts = INT64_MAX;
		for (int i = 0; i < RWD_MAX_CLIENTS; i++) {
			if (srv->clients[i] && srv->clients[i]->created_at < oldest_ts) {
				oldest_ts = srv->clients[i]->created_at;
				oldest_idx = i;
			}
		}
		if (oldest_idx < 0) {
			pthread_mutex_unlock(&srv->clients_lock);
			return false;
		}
		rwd_client_t *old = srv->clients[oldest_idx];
		RSS_WARN("WHIP: evicting oldest session %s to make room", old->session_id);
		rwd_media_teardown(old);
		rwd_dtls_client_free(old);
		free(old);
		srv->clients[oldest_idx] = NULL;
		srv->client_count--;
	}

	rwd_client_t *c = calloc(1, sizeof(*c));
	if (!c) {
		pthread_mutex_unlock(&srv->clients_lock);
		return false;
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
		return false;
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
		return false;
	}

	for (int i = 0; i < RWD_MAX_CLIENTS; i++) {
		if (!srv->clients[i]) {
			srv->clients[i] = c;
			srv->client_count++;
			break;
		}
	}
	/* The client belongs to the table from here on and any thread may
	 * free it: hand back a copy of what the caller needs, not the pointer. */
	memcpy(session_id_out, c->session_id, sizeof(c->session_id));
	pthread_mutex_unlock(&srv->clients_lock);

	RSS_DEBUG("client created: session %s", session_id_out);
	return true;
}

/* Send ICE checks to the candidates of a session created from an offer.
 * Runs under clients_lock so a concurrent teardown cannot free the client
 * mid-use; returns the number of candidates, or -1 if the session is gone. */
int rwd_session_punch(rwd_server_t *srv, const char *session_id)
{
	int n = -1;

	pthread_mutex_lock(&srv->clients_lock);
	for (int i = 0; i < RWD_MAX_CLIENTS; i++) {
		rwd_client_t *c = srv->clients[i];
		if (c && strcmp(c->session_id, session_id) == 0) {
			n = c->offer.candidate_count;
			for (int k = 0; k < n; k++)
				rwd_ice_send_check(srv, c, c->offer.candidates[k].ip,
						   c->offer.candidates[k].port);
			break;
		}
	}
	pthread_mutex_unlock(&srv->clients_lock);
	return n;
}

/* ── WHIP POST handler: SDP offer → answer ── */

static void handle_whip_post(rwd_server_t *srv, int fd, const char *body, size_t body_len,
			     const struct sockaddr_storage *local_addr, int stream_idx)
{
	(void)body_len;
	(void)local_addr;

	char sdp_answer[RWD_SDP_BUF_SIZE];
	rwd_sdp_reject_t reject = RWD_SDP_ERR_UNKNOWN;
	char session_id[RWD_SESSION_ID_LEN * 2 + 1];
	if (!rwd_client_from_offer(srv, body, stream_idx, sdp_answer, sizeof(sdp_answer), &reject,
				   session_id)) {
		/*
		 * Distinguish our failure from the caller's. Having no local
		 * address to put in the answer is a server fault even when the
		 * offer is perfectly good, and reporting it as 400 sends whoever
		 * is debugging off to inspect their own SDP.
		 */
		if (reject == RWD_SDP_ERR_NO_LOCAL_IP)
			http_error(fd, "500 Internal Server Error",
				   "cannot determine camera IP for SDP answer; "
				   "set local_ip in raptor.conf");
		else
			http_error(fd, "400 Bad Request", sdp_reject_reason(reject));
		return;
	}

	char extra[256];
	snprintf(extra, sizeof(extra), "Location: /whip/%s\r\nCache-Control: no-store\r\n",
		 session_id);
	http_send(fd, "201 Created", "application/sdp", sdp_answer, strlen(sdp_answer), extra);

	RSS_INFO("WHIP: session %s created", session_id);
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
	/* Workers hold the server: let them finish before it is torn down */
	for (int i = 0;
	     i < (RWD_SIGNALING_DEADLINE_MS + 1000) / 20 && atomic_load(&signaling_workers) > 0;
	     i++)
		usleep(20000);
	pthread_mutex_lock(&webrtc_html_lock);
	free(webrtc_html);
	webrtc_html = NULL;
	pthread_mutex_unlock(&webrtc_html_lock);
}

/* ── Main HTTP request handler ── */

void rwd_signaling_handle(rwd_server_t *srv, int client_fd,
			  const struct sockaddr_storage *local_addr)
{
	cors_enabled = srv->auth_user[0] && srv->auth_pass[0];
	req_deadline_us = rss_timestamp_us() + (int64_t)RWD_SIGNALING_DEADLINE_MS * 1000;
#ifdef RSS_HAS_TLS
	/* TLS handshake if HTTPS context available */
	tls_conn = NULL;
	if (srv->tls) {
		tls_conn = rss_tls_accept(srv->tls, client_fd, req_wait_ms());
		if (!tls_conn) {
			http_close(client_fd);
			return;
		}
		rss_tls_set_deadline(tls_conn, req_deadline_us);
	}
#endif

	/* Read HTTP request — may need multiple reads over TLS since
	 * headers and body can arrive in separate TLS records.
	 * Heap-allocated: 16KB on stack is too heavy for embedded. */
	char *buf = malloc(RWD_HTTP_BUF_SIZE);
	if (!buf) {
		http_close(client_fd);
		return;
	}
	ssize_t n = 0;
	while (n < (ssize_t)(RWD_HTTP_BUF_SIZE - 1)) {
		ssize_t r = http_read(client_fd, buf + n, RWD_HTTP_BUF_SIZE - 1 - n);
		if (r <= 0)
			break; /* closed, failed or out of time */
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
	}
	if (n <= 0)
		goto done;
	buf[n] = '\0';

	/* Parse request line */
	char method[16], path[256];
	if (sscanf(buf, "%15s %255s", method, path) != 2) {
		http_error(client_fd, "400 Bad Request", "malformed request line");
		goto done;
	}

	RSS_DEBUG("http: %s %s", method, path);

	/* Reject oversized requests */
	if (n >= (ssize_t)(RWD_HTTP_BUF_SIZE - 1)) {
		http_error(client_fd, "414 URI Too Long", "request exceeds buffer size");
		goto done;
	}

	/* CORS preflight */
	if (strcmp(method, "OPTIONS") == 0) {
		http_send(client_fd, "204 No Content", "text/plain", NULL, 0, NULL);
		goto done;
	}

	/* Basic auth check (if configured) */
	if (srv->auth_user[0] && srv->auth_pass[0]) {
		if (!rss_http_check_basic_auth(buf, srv->auth_user, srv->auth_pass)) {
			http_send(client_fd, "401 Unauthorized", "text/plain", "Unauthorized", 12,
				  "WWW-Authenticate: Basic realm=\"Raptor WebRTC\"\r\n");
			goto done;
		}
	}

	/* Strip query string for path matching */
	char *qs = strchr(path, '?');
	if (qs)
		*qs = '\0';

	/* GET /webrtc — serve player page */
	if (strcmp(method, "GET") == 0 && strcmp(path, "/webrtc") == 0) {
		pthread_mutex_lock(&webrtc_html_lock);
		if (!webrtc_html) {
			webrtc_html = rss_read_file(WEBRTC_HTML_PATH, &webrtc_html_len);
			if (webrtc_html)
				RSS_DEBUG("loaded %s (%d bytes)", WEBRTC_HTML_PATH,
					  webrtc_html_len);
			else
				RSS_WARN("%s not found", WEBRTC_HTML_PATH);
		}
		if (webrtc_html)
			http_send(client_fd, "200 OK", "text/html; charset=utf-8", webrtc_html,
				  (size_t)webrtc_html_len, "Cache-Control: no-cache\r\n");
		else
			http_send(client_fd, "404 Not Found", "text/plain", "player not installed",
				  20, NULL);
		pthread_mutex_unlock(&webrtc_html_lock);
		goto done;
	}

	/* POST /whip — SDP offer/answer */
	if (strcmp(method, "POST") == 0 && strncmp(path, "/whip", 5) == 0) {
		/* Parse ?stream=N from query string (qs was saved before NUL) */
		int stream_idx = 0;
		if (qs) {
			const char *sp = strstr(qs + 1, "stream=");
			if (sp) {
				int idx = sp[7] - '0';
				stream_idx = (idx >= 0 && idx < RWD_STREAM_COUNT) ? idx : 0;
			}
		}
		/* Parse Content-Length */
		size_t content_length = 0;
		const char *cl = strcasestr(buf, "Content-Length:");
		if (cl) {
			char *end;
			long cl_val = strtol(cl + 15, &end, 10);
			if (cl_val > 0 && cl_val < (long)RWD_HTTP_BUF_SIZE && end != cl + 15)
				content_length = (size_t)cl_val;
		}

		/* Find body (after \r\n\r\n) */
		const char *body = strstr(buf, "\r\n\r\n");
		if (!body) {
			RSS_WARN("WHIP: no body separator in request");
			http_error(client_fd, "400 Bad Request", "missing request body");
			goto done;
		}
		body += 4;
		size_t body_len = n - (body - buf);

		/* Read the rest of the body, within the request's deadline */
		size_t need = content_length > 0 ? content_length : 0;
		while (body_len < need && (size_t)n < RWD_HTTP_BUF_SIZE - 1) {
			ssize_t more = http_read(client_fd, buf + n, RWD_HTTP_BUF_SIZE - 1 - n);
			if (more <= 0)
				break;
			n += more;
			buf[n] = '\0';
			body_len = n - (body - buf);
		}

		handle_whip_post(srv, client_fd, body, body_len, local_addr, stream_idx);
		goto done;
	}

	/* DELETE /whip/{session} — teardown */
	if (strcmp(method, "DELETE") == 0 && strncmp(path, "/whip/", 6) == 0) {
		const char *session_id = path + 6;
		if (strlen(session_id) > 0) {
			handle_whip_delete(srv, client_fd, session_id);
			goto done;
		}
	}

	http_error(client_fd, "404 Not Found", "unknown path or method");

done:
	free(buf);
	http_close(client_fd);
}

struct signaling_job {
	rwd_server_t *srv;
	int fd;
	struct sockaddr_storage local;
};

static void *signaling_worker(void *arg)
{
	struct signaling_job *job = arg;
	rwd_signaling_handle(job->srv, job->fd, &job->local);
	free(job);
	atomic_fetch_sub(&signaling_workers, 1);
	return NULL;
}

void rwd_signaling_dispatch(rwd_server_t *srv, int client_fd,
			    const struct sockaddr_storage *local_addr)
{
	static int64_t last_warn;

	if (atomic_fetch_add(&signaling_workers, 1) >= RWD_SIGNALING_WORKERS) {
		atomic_fetch_sub(&signaling_workers, 1);
		int64_t now = rss_timestamp_us();
		if (now - last_warn > 10000000) {
			RSS_WARN("http: %d requests in progress, refusing connections",
				 RWD_SIGNALING_WORKERS);
			last_warn = now;
		}
		close(client_fd);
		return;
	}

	struct signaling_job *job = malloc(sizeof(*job));
	pthread_attr_t attr;
	pthread_t tid;
	bool started = false;
	if (job) {
		job->srv = srv;
		job->fd = client_fd;
		job->local = *local_addr;
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		pthread_attr_setstacksize(&attr, 256 * 1024);
		started = pthread_create(&tid, &attr, signaling_worker, job) == 0;
		pthread_attr_destroy(&attr);
	}
	if (!started) {
		RSS_WARN("http: cannot start a signaling worker");
		free(job);
		close(client_fd);
		atomic_fetch_sub(&signaling_workers, 1);
	}
}

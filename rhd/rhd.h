/*
 * rhd.h -- Raptor HTTP Daemon shared types and declarations
 */

#ifndef RHD_H
#define RHD_H

#include <stdbool.h>
#include <stdint.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <rss_ipc.h>
#include <rss_common.h>
#include <rss_net.h>
#include <rss_http.h>
#ifdef RSS_HAS_TLS
#include <rss_tls.h>
#endif

/* ── Constants ── */

#define RHD_MAX_CLIENTS	    8
#define RHD_RECV_BUF	    4096
#define RHD_MJPEG_BOUNDARY  "raptorframe"
#define RHD_SEND_TIMEOUT_MS 3000 /* max time to drain a one-shot response */
#define RHD_MAX_JPEG	    6	 /* up to 2 per sensor, 3 sensors */

/* Index page — loaded from file on first request, cached */
#define RHD_INDEX_PATH "/usr/share/raptor/index.html"

/* ── Types ── */

typedef struct {
	int fd;
	bool is_mjpeg; /* streaming MJPEG */
	struct sockaddr_storage addr;
	char recv_buf[RHD_RECV_BUF];
	size_t recv_len;

	/* Non-blocking send buffer (snapshot / one-shot responses) */
	uint8_t *send_buf;  /* heap-allocated response (header + body) */
	uint32_t send_len;  /* total bytes to send */
	uint32_t send_off;  /* bytes sent so far */
	int64_t send_start; /* monotonic timestamp for stall timeout */

#ifdef RSS_HAS_TLS
	rss_tls_conn_t *tls;
	rss_tls_ctx_t *srv_tls; /* server TLS context for lazy handshake */
#endif
} rhd_client_t;

typedef struct {
	int listen_fd;
	int epoll_fd;
	rss_config_t *cfg;
	const char *config_path;
	int port;
	int max_clients; /* runtime limit (≤ RHD_MAX_CLIENTS) */

	rhd_client_t *clients[RHD_MAX_CLIENTS];
	int client_count;

	/* JPEG rings for snapshot + MJPEG streaming */
	rss_ring_t *jpeg_rings[RHD_MAX_JPEG];
	int jpeg_ring_count;

	/* Snapshot read buffer (shared, single-threaded) */
	uint8_t *snap_buf;
	uint32_t snap_buf_size;

	volatile sig_atomic_t *running;
	rss_ctrl_t *ctrl;

	/* Basic auth (empty = no auth) */
	char auth_user[128];
	char auth_pass[128];

#ifdef RSS_HAS_TLS
	rss_tls_ctx_t *tls;
#endif
} rhd_server_t;

/* Address helpers — use shared rss_net.h */
#define client_addr_str rss_addr_str
#define client_port	rss_addr_port

/* ── HTTP I/O functions (rhd_http.c) ── */

ssize_t rhd_write(rhd_client_t *c, const void *buf, size_t len);
ssize_t rhd_read(rhd_client_t *c, void *buf, size_t len);
void http_send(rhd_client_t *c, const char *status, const char *content_type, const void *body,
	       int body_len);
void http_send_fd(int fd, const char *status, const char *content_type, const void *body,
		  int body_len);
int http_send_async(rhd_client_t *c, int epoll_fd, const char *content_type, const void *body,
		    uint32_t body_len);
void http_error(rhd_client_t *c, const char *status, const char *msg);
void http_401(rhd_client_t *c);
bool http_check_auth(const rhd_server_t *srv, const char *request);
void http_send_mjpeg_header(rhd_client_t *c);
int nb_write_all(rhd_client_t *c, const void *buf, size_t len);
int http_send_mjpeg_frame(rhd_client_t *c, const uint8_t *data, uint32_t len);

#endif /* RHD_H */

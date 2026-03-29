/*
 * rhd_main.c -- Raptor HTTP Daemon
 *
 * Minimal HTTP server for JPEG snapshots and MJPEG streaming.
 *
 * Endpoints:
 *   /snap.jpg   — latest JPEG snapshot (from JPEG ring)
 *   /mjpeg      — MJPEG stream (from jpeg ring)
 *
 * Dual-stack IPv6 (serves both IPv4 and IPv6 clients).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <poll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include <rss_ipc.h>
#include <rss_common.h>

#define RHD_MAX_CLIENTS	   8
#define RHD_RECV_BUF	   4096
#define RHD_MJPEG_BOUNDARY "raptorframe"

typedef struct {
	int fd;
	bool is_mjpeg; /* streaming MJPEG */
	char recv_buf[RHD_RECV_BUF];
	size_t recv_len;
} rhd_client_t;

#define RHD_MAX_JPEG 2

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
} rhd_server_t;

/* ── Base64 decode (RFC 4648) ── */

static const uint8_t b64_table[256] = {
	['A'] = 0,  ['B'] = 1,	['C'] = 2,  ['D'] = 3,	['E'] = 4,  ['F'] = 5,	['G'] = 6,
	['H'] = 7,  ['I'] = 8,	['J'] = 9,  ['K'] = 10, ['L'] = 11, ['M'] = 12, ['N'] = 13,
	['O'] = 14, ['P'] = 15, ['Q'] = 16, ['R'] = 17, ['S'] = 18, ['T'] = 19, ['U'] = 20,
	['V'] = 21, ['W'] = 22, ['X'] = 23, ['Y'] = 24, ['Z'] = 25, ['a'] = 26, ['b'] = 27,
	['c'] = 28, ['d'] = 29, ['e'] = 30, ['f'] = 31, ['g'] = 32, ['h'] = 33, ['i'] = 34,
	['j'] = 35, ['k'] = 36, ['l'] = 37, ['m'] = 38, ['n'] = 39, ['o'] = 40, ['p'] = 41,
	['q'] = 42, ['r'] = 43, ['s'] = 44, ['t'] = 45, ['u'] = 46, ['v'] = 47, ['w'] = 48,
	['x'] = 49, ['y'] = 50, ['z'] = 51, ['0'] = 52, ['1'] = 53, ['2'] = 54, ['3'] = 55,
	['4'] = 56, ['5'] = 57, ['6'] = 58, ['7'] = 59, ['8'] = 60, ['9'] = 61, ['+'] = 62,
	['/'] = 63,
};

static int base64_decode(const char *in, size_t in_len, char *out, size_t out_max)
{
	size_t out_len = 0;
	uint32_t buf = 0;
	int bits = 0;

	for (size_t i = 0; i < in_len && in[i] != '='; i++) {
		uint8_t v = b64_table[(uint8_t)in[i]];
		if (!v && in[i] != 'A')
			continue; /* skip invalid chars */
		buf = (buf << 6) | v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if (out_len >= out_max)
				return -1;
			out[out_len++] = (char)(buf >> bits);
		}
	}
	if (out_len < out_max)
		out[out_len] = '\0';
	return (int)out_len;
}

/* ── HTTP response helpers ── */

static void http_send(int fd, const char *status, const char *content_type, const void *body,
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
	write(fd, header, hlen);
	if (body && body_len > 0)
		write(fd, body, body_len);
}

static void http_error(int fd, const char *status, const char *msg)
{
	http_send(fd, status, "text/plain", msg, (int)strlen(msg));
}

static void http_401(int fd)
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
	write(fd, header, hlen);
	write(fd, body, strlen(body));
}

/* Check HTTP Basic auth. Returns true if authorized. */
static bool http_check_auth(const rhd_server_t *srv, const char *request)
{
	/* No auth configured — allow all */
	if (!srv->auth_user[0])
		return true;

	/* Search for header on a line boundary (after \n or at request start) */
	const char *needle = "Authorization: Basic ";
	const char *auth = NULL;
	const char *p = request;
	while ((p = strstr(p, needle)) != NULL) {
		if (p == request || *(p - 1) == '\n') {
			auth = p + strlen(needle);
			break;
		}
		p++;
	}
	if (!auth)
		return false;

	/* Find end of base64 value */
	const char *end = auth;
	while (*end && *end != '\r' && *end != '\n' && *end != ' ')
		end++;

	char decoded[256];
	int dlen = base64_decode(auth, (size_t)(end - auth), decoded, sizeof(decoded) - 1);
	if (dlen <= 0)
		return false;
	decoded[dlen] = '\0';

	/* decoded is "username:password" */
	char *colon = strchr(decoded, ':');
	if (!colon)
		return false;
	*colon = '\0';

	return rss_secure_compare(decoded, srv->auth_user) && rss_secure_compare(colon + 1, srv->auth_pass);
}

static void http_send_mjpeg_header(int fd)
{
	char header[256];
	int hlen = snprintf(header, sizeof(header),
			    "HTTP/1.1 200 OK\r\n"
			    "Content-Type: multipart/x-mixed-replace;boundary=" RHD_MJPEG_BOUNDARY
			    "\r\n"
			    "Cache-Control: no-cache\r\n"
			    "Access-Control-Allow-Origin: *\r\n"
			    "\r\n");
	write(fd, header, hlen);
}

/*
 * Write all bytes to a non-blocking fd, retrying on EAGAIN with poll().
 * Returns 0 on success, -1 if the client is dead or stalled beyond timeout.
 */
static int nb_write_all(int fd, const void *buf, size_t len)
{
	const uint8_t *p = buf;
	size_t remaining = len;
	int retries = 0;

	while (remaining > 0) {
		ssize_t n = write(fd, p, remaining);
		if (n > 0) {
			p += n;
			remaining -= (size_t)n;
			retries = 0;
		} else if (n < 0) {
			if ((errno == EAGAIN || errno == EWOULDBLOCK) && retries < 50) {
				/* Back off: poll for write-ready, 100ms max per retry */
				struct pollfd pfd = {.fd = fd, .events = POLLOUT};
				poll(&pfd, 1, 100);
				retries++;
			} else {
				return -1; /* dead or stalled too long */
			}
		}
	}
	return 0;
}

static int http_send_mjpeg_frame(int fd, const uint8_t *data, uint32_t len)
{
	char part_hdr[128];
	int hlen = snprintf(part_hdr, sizeof(part_hdr),
			    "--" RHD_MJPEG_BOUNDARY "\r\n"
			    "Content-Type: image/jpeg\r\n"
			    "Content-Length: %u\r\n"
			    "\r\n",
			    len);
	if (nb_write_all(fd, part_hdr, hlen) < 0)
		return -1;
	if (nb_write_all(fd, data, len) < 0)
		return -1;
	if (nb_write_all(fd, "\r\n", 2) < 0)
		return -1;
	return 0;
}

/* ── Snapshot handler — serve latest JPEG from ring ── */

static void handle_snapshot(int fd, rss_ring_t *ring, uint8_t *buf, uint32_t buf_size)
{
	if (!ring || !buf) {
		http_error(fd, "503 Service Unavailable", "JPEG ring not available");
		return;
	}

	/* Read the latest completed frame from the ring.
	 * Start at seq 0 to trigger EOVERFLOW which advances seq to
	 * the current write position, then back up by 1. */
	uint64_t seq = 0;
	uint32_t length;
	rss_ring_slot_t meta;
	int ret = rss_ring_read(ring, &seq, buf, buf_size, &length, &meta);

	if (ret == RSS_EOVERFLOW && seq > 0) {
		seq--;
		ret = rss_ring_read(ring, &seq, buf, buf_size, &length, &meta);
	}

	if (ret != 0 || length < 2 || buf[0] != 0xFF || buf[1] != 0xD8) {
		http_error(fd, "503 Service Unavailable", "No snapshot available yet");
		return;
	}

	http_send(fd, "200 OK", "image/jpeg", buf, (int)length);
}

/* ── Client management ── */

static void remove_client(rhd_server_t *srv, int idx)
{
	rhd_client_t *c = srv->clients[idx];
	epoll_ctl(srv->epoll_fd, EPOLL_CTL_DEL, c->fd, NULL);
	close(c->fd);
	free(c);
	srv->clients[idx] = srv->clients[--srv->client_count];
}

static int find_client(rhd_server_t *srv, int fd)
{
	for (int i = 0; i < srv->client_count; i++)
		if (srv->clients[i]->fd == fd)
			return i;
	return -1;
}

/* Parse ?stream=N query parameter, default 0 */
static int parse_stream_param(const char *path)
{
	const char *p = strstr(path, "stream=");
	if (p) {
		char *end;
		int v = (int)strtol(p + 7, &end, 10);
		if (end != p + 7 && v >= 0 && v < RHD_MAX_JPEG)
			return v;
	}
	return 0;
}

/* ── Request parsing ── */

static void handle_request(rhd_server_t *srv, rhd_client_t *c)
{
	/* Find request line */
	char *end = strstr(c->recv_buf, "\r\n\r\n");
	if (!end)
		return; /* incomplete */

	/* Parse method and path */
	char method[8] = {0}, path[128] = {0};
	sscanf(c->recv_buf, "%7s %127s", method, path);

	if (strcmp(method, "GET") != 0) {
		http_error(c->fd, "405 Method Not Allowed", "GET only");
		return;
	}

	if (!http_check_auth(srv, c->recv_buf)) {
		http_401(c->fd);
		return;
	}

	if (strncmp(path, "/snap", 5) == 0) {
		int si = parse_stream_param(path);
		if (si < srv->jpeg_ring_count && srv->jpeg_rings[si])
			handle_snapshot(c->fd, srv->jpeg_rings[si], srv->snap_buf,
					srv->snap_buf_size);
		else
			http_error(c->fd, "404 Not Found", "Stream not available");
	} else if (strncmp(path, "/mjpeg", 6) == 0 || strncmp(path, "/mjpg", 5) == 0) {
		/* Start MJPEG stream — don't close connection */
		http_send_mjpeg_header(c->fd);
		c->is_mjpeg = true;
		return; /* keep alive */
	} else if (strcmp(path, "/") == 0) {
		const char *html =
			"<html><head><title>Raptor</title></head>"
			"<body "
			"style=\"background:#1a1a1a;color:#ccc;font-family:sans-serif;margin:"
			"20px\">"
			"<h3 style=\"color:#fff\">Raptor</h3>"
			"<a style=\"color:#6af\" href=\"/snap.jpg\">Snapshot (main)</a> | "
			"<a style=\"color:#6af\" href=\"/snap.jpg?stream=1\">Snapshot (sub)</a> | "
			"<a style=\"color:#6af\" href=\"/mjpeg\">MJPEG Stream</a><br><br>"
			"<img src=\"/mjpeg\" style=\"max-width:100%;border:1px solid #333\">"
			"</body></html>";
		http_send(c->fd, "200 OK", "text/html", html, (int)strlen(html));
	} else {
		http_error(c->fd, "404 Not Found", "Not found");
	}
}

/* ── MJPEG streaming ── */

static void stream_mjpeg_frame(rhd_server_t *srv, const uint8_t *data, uint32_t len)
{
	for (int i = srv->client_count - 1; i >= 0; i--) {
		if (!srv->clients[i]->is_mjpeg)
			continue;
		if (http_send_mjpeg_frame(srv->clients[i]->fd, data, len) < 0)
			remove_client(srv, i);
	}
}

/* ── Server init ── */

/* ── Control socket ── */

static int rhd_ctrl_handler(const char *cmd_json, char *resp_buf, int resp_buf_size, void *userdata)
{
	rhd_server_t *srv = userdata;

	if (strstr(cmd_json, "\"config-get\"")) {
		char section[64], key[64];
		if (rss_json_get_str(cmd_json, "section", section, sizeof(section)) == 0 &&
		    rss_json_get_str(cmd_json, "key", key, sizeof(key)) == 0) {
			const char *v = rss_config_get_str(srv->cfg, section, key, NULL);
			if (v)
				snprintf(resp_buf, resp_buf_size, "%s", v);
			else
				resp_buf[0] = '\0';
		} else {
			resp_buf[0] = '\0';
		}
		return (int)strlen(resp_buf);
	}

	if (strstr(cmd_json, "\"config-save\"")) {
		int ret = rss_config_save(srv->cfg, srv->config_path);
		snprintf(resp_buf, resp_buf_size, "{\"status\":\"%s\"}", ret == 0 ? "ok" : "error");
		return (int)strlen(resp_buf);
	}

	if (strstr(cmd_json, "\"clients\"")) {
		int mjpeg = 0, snap = 0;
		for (int i = 0; i < srv->client_count; i++) {
			if (srv->clients[i]->is_mjpeg)
				mjpeg++;
			else
				snap++;
		}
		snprintf(resp_buf, resp_buf_size,
			 "{\"status\":\"ok\",\"clients\":%d,\"mjpeg\":%d,\"snapshot\":%d,"
			 "\"max_clients\":%d,\"jpeg_rings\":%d}",
			 srv->client_count, mjpeg, snap, srv->max_clients, srv->jpeg_ring_count);
		return (int)strlen(resp_buf);
	}

	/* Default: status */
	int mjpeg = 0;
	for (int i = 0; i < srv->client_count; i++)
		if (srv->clients[i]->is_mjpeg)
			mjpeg++;
	snprintf(resp_buf, resp_buf_size,
		 "{\"status\":\"ok\",\"clients\":%d,\"mjpeg\":%d,\"port\":%d,"
		 "\"jpeg_rings\":%d}",
		 srv->client_count, mjpeg, srv->port, srv->jpeg_ring_count);
	return (int)strlen(resp_buf);
}

static int server_init(rhd_server_t *srv)
{
	srv->listen_fd = socket(AF_INET6, SOCK_STREAM, 0);
	if (srv->listen_fd < 0) {
		RSS_FATAL("socket: %s", strerror(errno));
		return -1;
	}

	int opt = 1;
	setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	/* Dual-stack: accept both IPv4 and IPv6 */
	opt = 0;
	setsockopt(srv->listen_fd, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt));

	struct sockaddr_in6 addr = {
		.sin6_family = AF_INET6,
		.sin6_port = htons(srv->port),
		.sin6_addr = in6addr_any,
	};

	if (bind(srv->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		RSS_FATAL("bind port %d: %s", srv->port, strerror(errno));
		close(srv->listen_fd);
		return -1;
	}

	if (listen(srv->listen_fd, 8) < 0) {
		RSS_FATAL("listen: %s", strerror(errno));
		close(srv->listen_fd);
		return -1;
	}

	srv->epoll_fd = epoll_create1(0);
	struct epoll_event ev = {.events = EPOLLIN, .data.fd = srv->listen_fd};
	epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, srv->listen_fd, &ev);

	/* Control socket */
	rss_mkdir_p("/var/run/rss");
	srv->ctrl = rss_ctrl_listen("/var/run/rss/rhd.sock");
	if (srv->ctrl) {
		int ctrl_fd = rss_ctrl_get_fd(srv->ctrl);
		if (ctrl_fd >= 0) {
			ev = (struct epoll_event){.events = EPOLLIN, .data.fd = ctrl_fd};
			epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, ctrl_fd, &ev);
		}
	}

	RSS_INFO("HTTP server listening on port %d (dual-stack)", srv->port);
	return 0;
}

/* ── Main loop ── */

static void server_run(rhd_server_t *srv)
{
	uint64_t jpeg_read_seqs[RHD_MAX_JPEG] = {0};
	uint8_t *frame_buf = NULL;
	uint32_t frame_buf_size = 0;

	/* Try to open JPEG rings */
	for (int attempt = 0; attempt < 30 && *srv->running; attempt++) {
		for (int j = 0; j < RHD_MAX_JPEG; j++) {
			if (srv->jpeg_rings[j])
				continue;
			char name[16];
			snprintf(name, sizeof(name), "jpeg%d", j);
			srv->jpeg_rings[j] = rss_ring_open(name);
			if (srv->jpeg_rings[j]) {
				const rss_ring_header_t *hdr =
					rss_ring_get_header(srv->jpeg_rings[j]);
				RSS_INFO("%s ring available: %ux%u", name, hdr->width, hdr->height);
				srv->jpeg_ring_count++;
				if (hdr->data_size > frame_buf_size)
					frame_buf_size = hdr->data_size;
			}
		}
		if (srv->jpeg_ring_count > 0)
			break;
		RSS_DEBUG("waiting for jpeg rings...");
		sleep(1);
	}

	frame_buf = malloc(frame_buf_size);

	/* Snapshot buffer (shared with handle_request, single-threaded) */
	srv->snap_buf_size = frame_buf_size;
	srv->snap_buf = malloc(srv->snap_buf_size);

	struct epoll_event events[16];
	int ctrl_fd = srv->ctrl ? rss_ctrl_get_fd(srv->ctrl) : -1;

	while (*srv->running) {
		/* Check for new JPEG frames from ring 0 for MJPEG streaming */
		bool has_mjpeg_clients = false;
		for (int i = 0; i < srv->client_count; i++)
			if (srv->clients[i]->is_mjpeg) {
				has_mjpeg_clients = true;
				break;
			}

		if (has_mjpeg_clients && srv->jpeg_rings[0] && frame_buf) {
			uint32_t len;
			rss_ring_slot_t meta;
			int ret = rss_ring_read(srv->jpeg_rings[0], &jpeg_read_seqs[0], frame_buf,
						frame_buf_size, &len, &meta);
			if (ret == RSS_EOVERFLOW && jpeg_read_seqs[0] > 0) {
				/* Fell behind — back up 1 to read latest completed frame */
				jpeg_read_seqs[0]--;
				ret = rss_ring_read(srv->jpeg_rings[0], &jpeg_read_seqs[0],
						    frame_buf, frame_buf_size, &len, &meta);
			}
			if (ret == 0 && len >= 2 && frame_buf[0] == 0xFF && frame_buf[1] == 0xD8) {
				stream_mjpeg_frame(srv, frame_buf, len);
			} else if (ret == RSS_EOVERFLOW) {
				/* still overflow after retry — skip */
			}
		}

		int timeout = has_mjpeg_clients ? 50 : 500;
		int n = epoll_wait(srv->epoll_fd, events, 16, timeout);

		for (int i = 0; i < n; i++) {
			int fd = events[i].data.fd;

			if (fd == ctrl_fd) {
				rss_ctrl_accept_and_handle(srv->ctrl, rhd_ctrl_handler, srv);
				continue;
			}

			if (fd == srv->listen_fd) {
				/* Accept new client */
				struct sockaddr_storage sa;
				socklen_t salen = sizeof(sa);
				int cfd = accept(srv->listen_fd, (struct sockaddr *)&sa, &salen);
				if (cfd < 0)
					continue;

				if (srv->client_count >= srv->max_clients) {
					http_error(cfd, "503 Service Unavailable",
						   "Too many clients");
					close(cfd);
					continue;
				}

				int one = 1;
				setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
				int flags = fcntl(cfd, F_GETFL);
				if (flags >= 0)
					fcntl(cfd, F_SETFL, flags | O_NONBLOCK);

				rhd_client_t *c = calloc(1, sizeof(*c));
				c->fd = cfd;
				srv->clients[srv->client_count++] = c;

				struct epoll_event cev = {.events = EPOLLIN, .data.fd = cfd};
				epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, cfd, &cev);
				continue;
			}

			/* Client data */
			int ci = find_client(srv, fd);
			if (ci < 0)
				continue;

			rhd_client_t *c = srv->clients[ci];

			if (events[i].events & (EPOLLHUP | EPOLLERR)) {
				remove_client(srv, ci);
				continue;
			}

			size_t space = sizeof(c->recv_buf) - c->recv_len - 1;
			if (space == 0) {
				/* Request too large — reject */
				http_error(c->fd, "414 URI Too Long", "Request too large");
				remove_client(srv, ci);
				continue;
			}
			ssize_t nr = read(c->fd, c->recv_buf + c->recv_len, space);
			if (nr <= 0) {
				remove_client(srv, ci);
				continue;
			}
			c->recv_len += nr;
			c->recv_buf[c->recv_len] = '\0';

			/* Check for complete HTTP request */
			if (strstr(c->recv_buf, "\r\n\r\n")) {
				handle_request(srv, c);
				/* Close non-streaming connections */
				if (!c->is_mjpeg)
					remove_client(srv, ci);
			}
		}

		/* If we have MJPEG clients, wait for next frame from ring */
		if (has_mjpeg_clients && srv->jpeg_rings[0]) {
			rss_ring_wait(srv->jpeg_rings[0], 100);
		}
	}

	/* Cleanup */
	for (int i = srv->client_count - 1; i >= 0; i--)
		remove_client(srv, i);

	free(frame_buf);
	free(srv->snap_buf);
	for (int j = 0; j < RHD_MAX_JPEG; j++) {
		if (srv->jpeg_rings[j])
			rss_ring_close(srv->jpeg_rings[j]);
	}
	if (srv->ctrl)
		rss_ctrl_destroy(srv->ctrl);
	close(srv->listen_fd);
	close(srv->epoll_fd);
}

/* ── Entry point ── */

int main(int argc, char **argv)
{
	rss_daemon_ctx_t ctx;
	int ret = rss_daemon_init(&ctx, "rhd", argc, argv);
	if (ret != 0)
		return ret < 0 ? 1 : 0;

	if (!rss_config_get_bool(ctx.cfg, "http", "enabled", true)) {
		RSS_INFO("HTTP disabled in config");
		rss_config_free(ctx.cfg);
		rss_daemon_cleanup("rhd");
		return 0;
	}

	rhd_server_t srv = {0};
	srv.cfg = ctx.cfg;
	srv.config_path = ctx.config_path;
	srv.running = ctx.running;
	srv.port = rss_config_get_int(ctx.cfg, "http", "port", 8080);
	srv.max_clients = rss_config_get_int(ctx.cfg, "http", "max_clients", RHD_MAX_CLIENTS);
	if (srv.max_clients < 1) srv.max_clients = 1;
	if (srv.max_clients > RHD_MAX_CLIENTS) srv.max_clients = RHD_MAX_CLIENTS;

	/* Basic auth — enabled when both username and password are set */
	const char *http_user = rss_config_get_str(ctx.cfg, "http", "username", "");
	const char *http_pass = rss_config_get_str(ctx.cfg, "http", "password", "");
	if (http_user[0] && http_pass[0]) {
		strncpy(srv.auth_user, http_user, sizeof(srv.auth_user) - 1);
		strncpy(srv.auth_pass, http_pass, sizeof(srv.auth_pass) - 1);
		RSS_INFO("HTTP Basic auth enabled");
	}

	if (server_init(&srv) < 0) {
		rss_config_free(ctx.cfg);
		return 1;
	}

	server_run(&srv);

	RSS_INFO("rhd shutting down");
	rss_config_free(ctx.cfg);
	rss_daemon_cleanup("rhd");

	return 0;
}

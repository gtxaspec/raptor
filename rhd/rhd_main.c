/*
 * rhd_main.c -- Raptor HTTP Daemon
 *
 * Minimal HTTP server for JPEG snapshots and MJPEG streaming.
 *
 * Endpoints:
 *   /snap.jpg   — latest JPEG snapshot (from file)
 *   /mjpeg      — MJPEG stream (from jpeg ring)
 *
 * Dual-stack IPv6 (serves both IPv4 and IPv6 clients).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/epoll.h>
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
	char snap_paths[RHD_MAX_JPEG][64];
	int snap_count;
	int port;

	rhd_client_t *clients[RHD_MAX_CLIENTS];
	int client_count;

	/* JPEG rings for MJPEG streaming */
	rss_ring_t *jpeg_rings[RHD_MAX_JPEG];
	int jpeg_ring_count;

	volatile sig_atomic_t *running;
} rhd_server_t;

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

static int http_send_mjpeg_frame(int fd, const uint8_t *data, uint32_t len)
{
	char part_hdr[128];
	int hlen = snprintf(part_hdr, sizeof(part_hdr),
			    "--" RHD_MJPEG_BOUNDARY "\r\n"
			    "Content-Type: image/jpeg\r\n"
			    "Content-Length: %u\r\n"
			    "\r\n",
			    len);
	if (write(fd, part_hdr, hlen) < 0)
		return -1;
	if (write(fd, data, len) < 0)
		return -1;
	if (write(fd, "\r\n", 2) < 0)
		return -1;
	return 0;
}

/* ── Snapshot handler ── */

static void handle_snapshot(int fd, const char *snap_path)
{
	int file_size;
	char *data = rss_read_file(snap_path, &file_size);
	if (!data || file_size <= 0) {
		http_error(fd, "503 Service Unavailable", "No snapshot available yet");
		free(data);
		return;
	}
	http_send(fd, "200 OK", "image/jpeg", data, file_size);
	free(data);
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
		int v = atoi(p + 7);
		if (v >= 0 && v < RHD_MAX_JPEG)
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

	if (strncmp(path, "/snap", 5) == 0) {
		int si = parse_stream_param(path);
		if (si < srv->snap_count)
			handle_snapshot(c->fd, srv->snap_paths[si]);
		else
			http_error(c->fd, "404 Not Found", "Stream not available");
	} else if (strncmp(path, "/mjpeg", 6) == 0 || strncmp(path, "/mjpg", 5) == 0) {
		/* Start MJPEG stream — don't close connection */
		http_send_mjpeg_header(c->fd);
		c->is_mjpeg = true;
		return; /* keep alive */
	} else if (strcmp(path, "/") == 0) {
		const char *html = "<html><body>"
				   "<h3>Raptor</h3>"
				   "<a href=\"/snap.jpg\">Snapshot (main)</a><br>"
				   "<a href=\"/snap.jpg?stream=1\">Snapshot (sub)</a><br>"
				   "<a href=\"/mjpeg\">MJPEG Stream</a><br>"
				   "<img src=\"/mjpeg\" width=\"640\">"
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

	RSS_INFO("HTTP server listening on port %d (dual-stack)", srv->port);
	return 0;
}

/* ── Main loop ── */

static void server_run(rhd_server_t *srv)
{
	uint64_t jpeg_read_seqs[RHD_MAX_JPEG] = {0};
	uint8_t *frame_buf = NULL;
	uint32_t frame_buf_size = 256 * 1024;

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
				uint32_t slot_sz = hdr->data_size / hdr->slot_count;
				if (slot_sz > frame_buf_size)
					frame_buf_size = slot_sz;
			}
		}
		if (srv->jpeg_ring_count > 0)
			break;
		RSS_DEBUG("waiting for jpeg rings...");
		sleep(1);
	}

	frame_buf = malloc(frame_buf_size);

	struct epoll_event events[16];

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
			if (ret == 0) {
				stream_mjpeg_frame(srv, frame_buf, len);
			} else if (ret == RSS_EOVERFLOW) {
				/* skip to latest */
			}
		}

		int timeout = has_mjpeg_clients ? 50 : 500;
		int n = epoll_wait(srv->epoll_fd, events, 16, timeout);

		for (int i = 0; i < n; i++) {
			int fd = events[i].data.fd;

			if (fd == srv->listen_fd) {
				/* Accept new client */
				struct sockaddr_storage sa;
				socklen_t salen = sizeof(sa);
				int cfd = accept(srv->listen_fd, (struct sockaddr *)&sa, &salen);
				if (cfd < 0)
					continue;

				if (srv->client_count >= RHD_MAX_CLIENTS) {
					http_error(cfd, "503 Service Unavailable",
						   "Too many clients");
					close(cfd);
					continue;
				}

				int one = 1;
				setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

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

			ssize_t nr = read(c->fd, c->recv_buf + c->recv_len,
					  sizeof(c->recv_buf) - c->recv_len - 1);
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
	for (int j = 0; j < RHD_MAX_JPEG; j++) {
		if (srv->jpeg_rings[j])
			rss_ring_close(srv->jpeg_rings[j]);
	}
	close(srv->listen_fd);
	close(srv->epoll_fd);
}

/* ── Entry point ── */

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"  -c <file>   Config file (default: /etc/raptor.conf)\n"
		"  -f          Run in foreground\n"
		"  -d          Debug logging\n"
		"  -h          Show this help\n",
		prog);
}

int main(int argc, char **argv)
{
	const char *config_path = "/etc/raptor.conf";
	bool foreground = false;
	bool debug = false;
	int opt;

	while ((opt = getopt(argc, argv, "c:fdh")) != -1) {
		switch (opt) {
		case 'c':
			config_path = optarg;
			break;
		case 'f':
			foreground = true;
			break;
		case 'd':
			debug = true;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	rss_log_init("rhd", debug ? RSS_LOG_DEBUG : RSS_LOG_INFO,
		     foreground ? RSS_LOG_TARGET_STDERR : RSS_LOG_TARGET_SYSLOG, NULL);

	rss_config_t *cfg = rss_config_load(config_path);
	if (!cfg) {
		RSS_FATAL("failed to load config: %s", config_path);
		return 1;
	}

	if (!foreground) {
		if (rss_daemonize("rhd", false) < 0) {
			RSS_FATAL("daemonize failed");
			rss_config_free(cfg);
			return 1;
		}
	}

	volatile sig_atomic_t *running = rss_signal_init();
	RSS_INFO("rhd starting");

	rhd_server_t srv = {0};
	srv.cfg = cfg;
	srv.config_path = config_path;
	srv.running = running;
	srv.port = rss_config_get_int(cfg, "http", "port", 8080);
	for (int j = 0; j < RHD_MAX_JPEG; j++)
		snprintf(srv.snap_paths[j], sizeof(srv.snap_paths[j]), "/tmp/snapshot-%d.jpg", j);
	srv.snap_count = RHD_MAX_JPEG;

	if (server_init(&srv) < 0) {
		rss_config_free(cfg);
		return 1;
	}

	server_run(&srv);

	RSS_INFO("rhd shutting down");
	rss_config_free(cfg);
	if (!foreground)
		rss_daemon_cleanup("rhd");

	return 0;
}

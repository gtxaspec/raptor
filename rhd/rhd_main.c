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
#include <time.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <poll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <stdatomic.h>

#include "rhd.h"

/* Index page — loaded from file on first request, cached */
static char *index_html;
static int index_html_len;

/* Base64 and HTTP auth moved to raptor-common (rss_http.h) */

/* ── Snapshot handler — serve latest JPEG from ring ── */

static bool handle_snapshot(rhd_client_t *c, int epoll_fd, rss_ring_t *ring, uint8_t *buf,
			    uint32_t buf_size)
{
	if (!ring || !buf) {
		http_error(c, "503 Service Unavailable", "JPEG ring not available");
		return false;
	}

	/* Signal demand so the JPEG encoder starts if idle */
	rss_ring_acquire(ring);

	/* Skip stale frames — start past current write_seq so we only
	 * get a frame produced AFTER the acquire woke the encoder.
	 * JPEG may be 1 fps, so allow up to 2s for the first frame. */
	const rss_ring_header_t *hdr = rss_ring_get_header(ring);
	uint64_t seq = atomic_load(&hdr->write_seq);
	uint32_t length = 0;
	rss_ring_slot_t meta;
	int ret = -EAGAIN;

	for (int attempt = 0; attempt < 20; attempt++) {
		rss_ring_wait(ring, 100);
		ret = rss_ring_read(ring, &seq, buf, buf_size, &length, &meta);
		if (ret == 0 && length >= 2 && buf[0] == 0xFF && buf[1] == 0xD8)
			break;
	}

	rss_ring_release(ring);

	if (ret != 0 || length < 2 || buf[0] != 0xFF || buf[1] != 0xD8) {
		http_error(c, "503 Service Unavailable", "No snapshot available yet");
		return false;
	}

	/* Queue for non-blocking send via epoll */
	if (http_send_async(c, epoll_fd, "image/jpeg", buf, length) < 0) {
		http_error(c, "500 Internal Server Error", "Out of memory");
		return false;
	}
	return true; /* keep alive — epoll will drain and close */
}

/* ── Client management ── */

static void remove_client(rhd_server_t *srv, int idx)
{
	rhd_client_t *c = srv->clients[idx];
	char addrstr[INET6_ADDRSTRLEN];
	RSS_INFO("client %s:%u disconnected%s", client_addr_str(&c->addr, addrstr, sizeof(addrstr)),
		 client_port(&c->addr), c->is_mjpeg ? " (mjpeg)" : "");
	epoll_ctl(srv->epoll_fd, EPOLL_CTL_DEL, c->fd, NULL);
#ifdef RSS_HAS_TLS
	rss_tls_close(c->tls);
#endif
	close(c->fd);
	free(c->send_buf);
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

	char addrstr[INET6_ADDRSTRLEN];
	RSS_INFO("%s %s from %s:%u", method, path,
		 client_addr_str(&c->addr, addrstr, sizeof(addrstr)), client_port(&c->addr));

	if (strcmp(method, "GET") != 0) {
		http_error(c, "405 Method Not Allowed", "GET only");
		return;
	}

	if (!http_check_auth(srv, c->recv_buf)) {
		http_401(c);
		return;
	}

	if (strncmp(path, "/snap", 5) == 0) {
		int si = parse_stream_param(path);
		if (si < srv->jpeg_ring_count && srv->jpeg_rings[si]) {
			if (handle_snapshot(c, srv->epoll_fd, srv->jpeg_rings[si], srv->snap_buf,
					    srv->snap_buf_size))
				return; /* keep alive — async send in progress */
		} else {
			http_error(c, "404 Not Found", "Stream not available");
		}
	} else if (strncmp(path, "/mjpeg", 6) == 0 || strncmp(path, "/mjpg", 5) == 0) {
		/* Start MJPEG stream — don't close connection */
		http_send_mjpeg_header(c);
		c->is_mjpeg = true;
		return; /* keep alive */
	} else if (strcmp(path, "/audio") == 0) {
		if (!srv->audio_ring) {
			http_error(c, "404 Not Found", "Audio not available");
		} else {
			if (rhd_audio_send_header(c, srv->audio_codec, srv->audio_sample_rate) < 0)
				return;
			c->is_audio = true;
			return; /* keep alive */
		}
	} else if (strcmp(path, "/") == 0) {
		if (!index_html) {
			index_html = rss_read_file(RHD_INDEX_PATH, &index_html_len);
			if (index_html)
				RSS_DEBUG("loaded %s (%d bytes)", RHD_INDEX_PATH, index_html_len);
			else
				RSS_WARN("%s not found", RHD_INDEX_PATH);
		}
		if (index_html)
			http_send(c, "200 OK", "text/html", index_html, index_html_len);
		else
			http_error(c, "404 Not Found", "index page not installed");
	} else {
		http_error(c, "404 Not Found", "Not found");
	}
}

/* ── MJPEG streaming ── */

static void stream_mjpeg_frame(rhd_server_t *srv, const uint8_t *data, uint32_t len)
{
	for (int i = srv->client_count - 1; i >= 0; i--) {
		if (!srv->clients[i]->is_mjpeg)
			continue;
		if (http_send_mjpeg_frame(srv->clients[i], data, len) < 0)
			remove_client(srv, i);
	}
}

/* ── Server init ── */

/* ── Control socket ── */

static int rhd_ctrl_handler(const char *cmd_json, char *resp_buf, int resp_buf_size, void *userdata)
{
	rhd_server_t *srv = userdata;

	int rc = rss_ctrl_handle_common(cmd_json, resp_buf, resp_buf_size, srv->cfg,
					srv->config_path);
	if (rc >= 0)
		return rc;

	if (strstr(cmd_json, "\"clients\"")) {
		int n = snprintf(resp_buf, resp_buf_size,
				 "{\"status\":\"ok\",\"count\":%d,\"max_clients\":%d,\"clients\":[",
				 srv->client_count, srv->max_clients);
		for (int i = 0; i < srv->client_count; i++) {
			rhd_client_t *c = srv->clients[i];
			char addr[INET6_ADDRSTRLEN];
			client_addr_str(&c->addr, addr, sizeof(addr));
			const char *type = c->is_mjpeg	 ? "mjpeg"
					   : c->is_audio ? "audio"
							 : "snapshot";
			n += snprintf(resp_buf + n, resp_buf_size - n,
				      "%s{\"ip\":\"%s\",\"type\":\"%s\"}", i > 0 ? "," : "", addr,
				      type);
		}
		snprintf(resp_buf + n, resp_buf_size - n, "]}");
		return (int)strlen(resp_buf);
	}

	/* Default: status */
	int mjpeg = 0, audio = 0;
	for (int i = 0; i < srv->client_count; i++) {
		if (srv->clients[i]->is_mjpeg)
			mjpeg++;
		if (srv->clients[i]->is_audio)
			audio++;
	}
	return rss_ctrl_resp(resp_buf, resp_buf_size,
			     "{\"status\":\"ok\",\"clients\":%d,\"mjpeg\":%d,"
			     "\"audio\":%d,\"port\":%d,\"jpeg_rings\":%d}",
			     srv->client_count, mjpeg, audio, srv->port, srv->jpeg_ring_count);
}

static int server_init(rhd_server_t *srv)
{
	srv->listen_fd = rss_listen_tcp(srv->port, 8);
	if (srv->listen_fd < 0) {
		RSS_FATAL("listen on port %d: %s", srv->port, strerror(errno));
		return -1;
	}

	srv->epoll_fd = epoll_create1(0);
	struct epoll_event ev = {.events = EPOLLIN, .data.fd = srv->listen_fd};
	if (epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, srv->listen_fd, &ev) < 0)
		RSS_ERROR("epoll_ctl add listen_fd: %s", strerror(errno));

	/* Control socket */
	rss_mkdir_p("/var/run/rss");
	srv->ctrl = rss_ctrl_listen("/var/run/rss/rhd.sock");
	if (srv->ctrl) {
		int ctrl_fd = rss_ctrl_get_fd(srv->ctrl);
		if (ctrl_fd >= 0) {
			ev = (struct epoll_event){.events = EPOLLIN, .data.fd = ctrl_fd};
			if (epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, ctrl_fd, &ev) < 0)
				RSS_ERROR("epoll_ctl add ctrl_fd: %s", strerror(errno));
		}
	}

	RSS_INFO("HTTP server listening on port %d (dual-stack)", srv->port);
	return 0;
}

/* ── Main loop ── */

static void server_run(rhd_server_t *srv)
{
	uint64_t jpeg_read_seqs[RHD_MAX_JPEG] = {0};
	uint64_t jpeg_last_ws[RHD_MAX_JPEG] = {0};
	int jpeg_idle[RHD_MAX_JPEG] = {0};
	int jpeg_reconnect_tick = 0;

	/* Audio ring state */
	uint64_t audio_read_seq = 0;
	uint8_t audio_buf[4096];
	uint8_t *frame_buf = NULL;
	uint32_t frame_buf_size = 0;

	/* JPEG ring names: sensor 0 = jpeg0/jpeg1, sensor N = sN_jpeg0/sN_jpeg1 */
	static const char *jpeg_ring_names[RHD_MAX_JPEG] = {"jpeg0",	"jpeg1",    "s1_jpeg0",
							    "s1_jpeg1", "s2_jpeg0", "s2_jpeg1"};

	/* Try to open JPEG rings */
	for (int attempt = 0; attempt < 30 && *srv->running; attempt++) {
		for (int j = 0; j < RHD_MAX_JPEG; j++) {
			if (srv->jpeg_rings[j])
				continue;
			const char *name = jpeg_ring_names[j];
			srv->jpeg_rings[j] = rss_ring_open(name);
			if (srv->jpeg_rings[j]) {
				const rss_ring_header_t *hdr =
					rss_ring_get_header(srv->jpeg_rings[j]);
				RSS_DEBUG("%s ring available: %ux%u", name, hdr->width,
					  hdr->height);
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

	/* Try to open audio ring */
	srv->audio_ring = rss_ring_open("audio");
	if (srv->audio_ring) {
		const rss_ring_header_t *ahdr = rss_ring_get_header(srv->audio_ring);
		srv->audio_codec = ahdr->codec;
		srv->audio_sample_rate = ahdr->fps_num;
		RSS_INFO("audio ring available (codec=%d rate=%d)", srv->audio_codec,
			 srv->audio_sample_rate);
	}

	frame_buf = malloc(frame_buf_size);
	if (!frame_buf) {
		RSS_FATAL("failed to allocate frame buffer (%u bytes)", frame_buf_size);
		return;
	}

	/* Snapshot buffer (shared with handle_request, single-threaded) */
	srv->snap_buf_size = frame_buf_size;
	srv->snap_buf = malloc(srv->snap_buf_size);
	if (!srv->snap_buf) {
		RSS_FATAL("failed to allocate snapshot buffer (%u bytes)", srv->snap_buf_size);
		free(frame_buf);
		return;
	}

	struct epoll_event events[16];
	int ctrl_fd = srv->ctrl ? rss_ctrl_get_fd(srv->ctrl) : -1;

	bool was_streaming = false;

	while (*srv->running) {
		/* Check for new JPEG frames from ring 0 for MJPEG streaming */
		bool has_mjpeg_clients = false;
		for (int i = 0; i < srv->client_count; i++)
			if (srv->clients[i]->is_mjpeg) {
				has_mjpeg_clients = true;
				break;
			}

		/* Signal demand to producer (JPEG encoder starts/stops based on this) */
		if (has_mjpeg_clients && !was_streaming) {
			for (int j = 0; j < RHD_MAX_JPEG; j++)
				if (srv->jpeg_rings[j])
					rss_ring_acquire(srv->jpeg_rings[j]);
			was_streaming = true;
		} else if (!has_mjpeg_clients && was_streaming) {
			for (int j = 0; j < RHD_MAX_JPEG; j++)
				if (srv->jpeg_rings[j])
					rss_ring_release(srv->jpeg_rings[j]);
			was_streaming = false;
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

		/* Stream audio frames to audio clients */
		bool has_audio_clients = false;
		for (int i = 0; i < srv->client_count; i++)
			if (srv->clients[i]->is_audio) {
				has_audio_clients = true;
				break;
			}

		if (has_audio_clients && srv->audio_ring) {
			/* Drain all available audio frames — audio arrives faster
			 * than the epoll loop period (20ms frames vs 50ms poll). */
			for (int af = 0; af < 20; af++) {
				uint32_t alen;
				rss_ring_slot_t meta;
				int ret = rss_ring_read(srv->audio_ring, &audio_read_seq, audio_buf,
							sizeof(audio_buf), &alen, &meta);
				if (ret == RSS_EOVERFLOW) {
					/* Jump to latest frame */
					const rss_ring_header_t *ahdr =
						rss_ring_get_header(srv->audio_ring);
					audio_read_seq =
						ahdr->write_seq > 0 ? ahdr->write_seq - 1 : 0;
					continue;
				}
				if (ret != 0 || alen == 0)
					break; /* no more frames */

				for (int i = srv->client_count - 1; i >= 0; i--) {
					if (!srv->clients[i]->is_audio)
						continue;
					rhd_client_t *ac = srv->clients[i];
					if (rhd_audio_send_frame(ac, srv->audio_codec,
								 srv->audio_sample_rate, audio_buf,
								 alen, ac->audio_page_seq,
								 ac->audio_granule) < 0) {
						remove_client(srv, i);
						continue;
					}
					ac->audio_page_seq++;
					/* Opus: 960 samples per 20ms frame at 48kHz.
					 * PCM/G.711: bytes / 2 = samples (16-bit mono).
					 * AAC: 1024 samples per frame. */
					if (srv->audio_codec == RHD_CODEC_OPUS)
						ac->audio_granule += 960;
					else if (srv->audio_codec == RHD_CODEC_AAC)
						ac->audio_granule += 1024;
					else
						ac->audio_granule += alen / 2;
				}
			}
		}

		int timeout = (has_mjpeg_clients || has_audio_clients) ? 50 : 500;
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
					char addrstr[INET6_ADDRSTRLEN];
					RSS_WARN("rejected %s:%u (max clients %d)",
						 client_addr_str(&sa, addrstr, sizeof(addrstr)),
						 client_port(&sa), srv->max_clients);
					http_send_fd(cfd, "503 Service Unavailable", "text/plain",
						     "Too many clients", 16);
					close(cfd);
					continue;
				}

				int one = 1;
				setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
				int flags = fcntl(cfd, F_GETFL);
				if (flags >= 0)
					fcntl(cfd, F_SETFL, flags | O_NONBLOCK);

				rhd_client_t *c = calloc(1, sizeof(*c));
				if (!c) {
					RSS_ERROR("client alloc failed");
					close(cfd);
					continue;
				}
				c->fd = cfd;
				memcpy(&c->addr, &sa, sizeof(c->addr));
#ifdef RSS_HAS_TLS
				c->srv_tls = srv->tls;
#endif
				srv->clients[srv->client_count++] = c;

				char addrstr[INET6_ADDRSTRLEN];
				RSS_INFO("client %s:%u connected (%d/%d)",
					 client_addr_str(&sa, addrstr, sizeof(addrstr)),
					 client_port(&sa), srv->client_count, srv->max_clients);

				struct epoll_event cev = {.events = EPOLLIN, .data.fd = cfd};
				if (epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, cfd, &cev) < 0) {
					RSS_ERROR("epoll_ctl add client fd: %s", strerror(errno));
					srv->client_count--;
					free(c);
					close(cfd);
				}
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

			/* Async send in progress — drain via EPOLLOUT */
			if (c->send_buf && (events[i].events & EPOLLOUT)) {
				uint32_t remain = c->send_len - c->send_off;
				ssize_t nw = rhd_write(c, c->send_buf + c->send_off, remain);
				if (nw > 0) {
					c->send_off += (uint32_t)nw;
					if (c->send_off >= c->send_len) {
						/* Done — close connection */
						remove_client(srv, ci);
					}
				} else if (nw < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
					remove_client(srv, ci);
				}
				continue;
			}

			size_t space = sizeof(c->recv_buf) - c->recv_len - 1;
			if (space == 0) {
				/* Request too large — reject */
				http_error(c, "414 URI Too Long", "Request too large");
				remove_client(srv, ci);
				continue;
			}
			ssize_t nr = rhd_read(c, c->recv_buf + c->recv_len, space);
			if (nr <= 0) {
				remove_client(srv, ci);
				continue;
			}
			c->recv_len += nr;
			c->recv_buf[c->recv_len] = '\0';

			/* Check for complete HTTP request */
			if (strstr(c->recv_buf, "\r\n\r\n")) {
				handle_request(srv, c);
				/* Close non-streaming, non-async connections */
				if (!c->is_mjpeg && !c->is_audio && !c->send_buf)
					remove_client(srv, ci);
			}
		}

		/* Reap stalled async sends */
		{
			struct timespec ts;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			int64_t now = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
			for (int i = srv->client_count - 1; i >= 0; i--) {
				rhd_client_t *c = srv->clients[i];
				if (c->send_buf && (now - c->send_start) > RHD_SEND_TIMEOUT_MS)
					remove_client(srv, i);
			}
		}

		/* Wait for next frame from active rings */
		if (has_audio_clients && srv->audio_ring)
			rss_ring_wait(srv->audio_ring, 20);
		else if (has_mjpeg_clients && srv->jpeg_rings[0])
			rss_ring_wait(srv->jpeg_rings[0], 100);

		/* Periodic JPEG ring reconnection (~every 2s) */
		if (++jpeg_reconnect_tick >= 20) {
			jpeg_reconnect_tick = 0;
			for (int j = 0; j < RHD_MAX_JPEG; j++) {
				if (!srv->jpeg_rings[j]) {
					srv->jpeg_rings[j] = rss_ring_open(jpeg_ring_names[j]);
					if (srv->jpeg_rings[j]) {
						if (was_streaming)
							rss_ring_acquire(srv->jpeg_rings[j]);
						jpeg_read_seqs[j] = 0;
						const rss_ring_header_t *hdr =
							rss_ring_get_header(srv->jpeg_rings[j]);
						if (hdr->data_size > frame_buf_size) {
							free(frame_buf);
							frame_buf_size = hdr->data_size;
							frame_buf = malloc(frame_buf_size);
							if (!frame_buf)
								frame_buf_size = 0;
						}
						RSS_DEBUG("jpeg ring reconnected (%s)",
							  jpeg_ring_names[j]);
					}
					continue;
				}
				const rss_ring_header_t *hdr =
					rss_ring_get_header(srv->jpeg_rings[j]);
				uint64_t ws = hdr->write_seq;
				if (ws == jpeg_last_ws[j])
					jpeg_idle[j]++;
				else
					jpeg_idle[j] = 0;
				jpeg_last_ws[j] = ws;
				if (jpeg_idle[j] >= 10) { /* ~20s (10 ticks * 2s/tick) */
					RSS_DEBUG("jpeg ring idle, closing (%s)",
						  jpeg_ring_names[j]);
					if (was_streaming)
						rss_ring_release(srv->jpeg_rings[j]);
					rss_ring_close(srv->jpeg_rings[j]);
					srv->jpeg_rings[j] = NULL;
					jpeg_idle[j] = 0;
				}
			}
		}
	}

	/* Cleanup */
	for (int i = srv->client_count - 1; i >= 0; i--)
		remove_client(srv, i);

	free(frame_buf);
	free(srv->snap_buf);
	for (int j = 0; j < RHD_MAX_JPEG; j++) {
		if (srv->jpeg_rings[j]) {
			if (was_streaming)
				rss_ring_release(srv->jpeg_rings[j]);
			rss_ring_close(srv->jpeg_rings[j]);
		}
	}
	if (srv->audio_ring)
		rss_ring_close(srv->audio_ring);
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
	RSS_BANNER("rhd");

	rss_base64_init();

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
	if (srv.max_clients < 1)
		srv.max_clients = 1;
	if (srv.max_clients > RHD_MAX_CLIENTS)
		srv.max_clients = RHD_MAX_CLIENTS;

	/* Basic auth — enabled when both username and password are set */
	const char *http_user = rss_config_get_str(ctx.cfg, "http", "username", "admin");
	const char *http_pass = rss_config_get_str(ctx.cfg, "http", "password", "secret");
	if (http_user[0] && http_pass[0]) {
		rss_strlcpy(srv.auth_user, http_user, sizeof(srv.auth_user));
		rss_strlcpy(srv.auth_pass, http_pass, sizeof(srv.auth_pass));
		RSS_INFO("HTTP Basic auth enabled");
	}

#ifdef RSS_HAS_TLS
	bool https = rss_config_get_bool(ctx.cfg, "http", "https", false);
	if (https) {
		const char *cert =
			rss_config_get_str(ctx.cfg, "http", "cert", "/etc/ssl/certs/uhttpd.crt");
		const char *key =
			rss_config_get_str(ctx.cfg, "http", "key", "/etc/ssl/private/uhttpd.key");
		srv.tls = rss_tls_init(cert, key);
		if (srv.tls)
			RSS_INFO("HTTPS enabled");
		else
			RSS_WARN("HTTPS init failed, falling back to HTTP");
	}
#endif

	if (server_init(&srv) < 0)
		goto cleanup;

	server_run(&srv);

	RSS_INFO("rhd shutting down");

cleanup:
	free(index_html);
#ifdef RSS_HAS_TLS
	rss_tls_free(srv.tls);
#endif
	rss_config_free(ctx.cfg);
	rss_daemon_cleanup("rhd");
	return 0;
}

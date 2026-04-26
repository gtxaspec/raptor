/*
 * rsd_server.c -- Epoll event loop, connection management
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <pthread.h>

#include "rsd.h"
#include <rss_net.h>

#define client_addr_str rss_addr_str
#define client_port	rss_addr_port

/* Called without clients_lock — safe because only the main thread
 * (epoll loop) modifies the client array. No concurrent writers. */
static rsd_client_t *find_client_by_fd(rsd_server_t *srv, int fd)
{
	for (int i = 0; i < srv->client_count; i++) {
		if (srv->clients[i] && srv->clients[i]->fd == fd)
			return srv->clients[i];
	}
	return NULL;
}

static rsd_client_t *find_client_by_rtcp_fd(rsd_server_t *srv, int fd)
{
	for (int i = 0; i < srv->client_count; i++) {
		if (srv->clients[i] && srv->clients[i]->udp_rtcp_fd == fd)
			return srv->clients[i];
	}
	return NULL;
}

static void remove_client(rsd_server_t *srv, rsd_client_t *client)
{
	if (!client)
		return;

	char addrstr[INET6_ADDRSTRLEN];
	RSS_INFO("client %s:%u disconnected",
		 client_addr_str(&client->addr, addrstr, sizeof(addrstr)),
		 client_port(&client->addr));

	/* Step 1: Remove from client list and stop playback under lock.
	 * This ensures the ring reader threads won't access this client's
	 * transports while we're freeing them. */
	pthread_mutex_lock(&srv->clients_lock);
	client->video.playing = false;
	client->audio.playing = false;
	for (int i = 0; i < srv->client_count; i++) {
		if (srv->clients[i] == client) {
			srv->clients[i] = srv->clients[srv->client_count - 1];
			srv->clients[srv->client_count - 1] = NULL;
			srv->client_count--;
			break;
		}
	}
	pthread_mutex_unlock(&srv->clients_lock);

	/* Step 2: Stop send thread — must complete before freeing transports.
	 * After step 1, the ring readers won't enqueue new frames (client is
	 * removed from list and playing=false). The send thread may be blocked
	 * on a TLS write — signal shutdown and wait for it to finish. */
	if (client->send_thread_running) {
		pthread_mutex_lock(&client->sendq.lock);
		client->sendq.shutdown = true;
		pthread_cond_signal(&client->sendq.cond);
		pthread_mutex_unlock(&client->sendq.lock);
		/* Unblock any in-progress TLS write so the send thread
		 * can observe shutdown promptly instead of waiting for
		 * TCP retransmit timeout on a dead connection. */
		shutdown(client->fd, SHUT_WR);
		pthread_join(client->send_tid, NULL);
		client->send_thread_running = false;
	}
	rsd_sendq_destroy(&client->sendq);
	pthread_mutex_destroy(&client->write_lock);

	/* Step 3: Now safe to free transports — ring readers and send thread are done */

	/* Send RTCP BYE */
	if (client->video.rtcp) {
		ssize_t bye_ret = Compy_Rtcp_send_bye(client->video.rtcp);
		(void)bye_ret;
	}

	/* Video cleanup */
	if (client->video.nal) {
		VCALL(DYN(Compy_NalTransport, Compy_Droppable, client->video.nal), drop);
		client->video.nal = NULL;
		client->video.rtp = NULL;
	}
	if (client->video.rtcp) {
		VCALL(DYN(Compy_Rtcp, Compy_Droppable, client->video.rtcp), drop);
		client->video.rtcp = NULL;
	}

	/* Audio cleanup */
	if (client->audio.rtp) {
		VCALL(DYN(Compy_RtpTransport, Compy_Droppable, client->audio.rtp), drop);
		client->audio.rtp = NULL;
	}
	if (client->audio.rtcp) {
		VCALL(DYN(Compy_Rtcp, Compy_Droppable, client->audio.rtcp), drop);
		client->audio.rtcp = NULL;
	}

	/* Backchannel cleanup */
	if (client->backchannel) {
		VCALL(DYN(Compy_Backchannel, Compy_Droppable, client->backchannel), drop);
		client->backchannel = NULL;
	}
	free(client->bc_recv);
	client->bc_recv = NULL;
	if (client->speaker_ring) {
		rss_ring_destroy(client->speaker_ring);
		client->speaker_ring = NULL;
	}

	/* Close UDP sockets if used */
	if (client->udp_rtp_fd >= 0)
		close(client->udp_rtp_fd);
	if (client->udp_rtcp_fd >= 0) {
		if (client->rtcp_in_epoll)
			epoll_ctl(srv->epoll_fd, EPOLL_CTL_DEL, client->udp_rtcp_fd, NULL);
		close(client->udp_rtcp_fd);
	}

	/* TLS shutdown and cleanup */
#ifdef COMPY_HAS_TLS
	if (client->tls) {
		compy_tls_shutdown(client->tls);
		Compy_TlsConn_free(client->tls);
		client->tls = NULL;
	}
#endif

	/* Remove from epoll and close TCP fd */
	epoll_ctl(srv->epoll_fd, EPOLL_CTL_DEL, client->fd, NULL);
	close(client->fd);

	free(client);
}

static void accept_client(rsd_server_t *srv)
{
	struct sockaddr_storage addr;
	socklen_t addrlen = sizeof(addr);
	int fd = accept(srv->listen_fd, (struct sockaddr *)&addr, &addrlen);
	if (fd < 0)
		return;

	/* Keep fd blocking for writes — non-blocking would drop RTP packets
	 * when the send buffer fills (78KB keyframes need multiple writes).
	 * Reads are handled by epoll with EPOLLIN. */
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

	/* TCP send buffer: smaller = lower latency (less queuing), but must
	 * fit at least one keyframe. 64KB is ~250ms at 2Mbps. */
	int sndbuf = srv->tcp_sndbuf;
	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

	rsd_client_t *client = calloc(1, sizeof(*client));
	if (!client) {
		close(fd);
		return;
	}
	client->srv = srv;
	client->udp_rtp_fd = -1;
	client->udp_rtcp_fd = -1;
	client->video_rtcp_ch = 0xFF; /* invalid until SETUP sets it */
	client->audio_rtcp_ch = 0xFF;

	client->fd = fd;
	client->addr = addr;
	client->active = true;
	client->waiting_keyframe = true;
	client->last_activity = rss_timestamp_us();

#ifdef COMPY_HAS_TLS
	if (srv->tls_ctx) {
		client->tls = Compy_TlsConn_accept(srv->tls_ctx, fd);
		if (!client->tls) {
			char addrstr[INET6_ADDRSTRLEN];
			RSS_WARN("TLS handshake failed from %s:%u",
				 client_addr_str(&addr, addrstr, sizeof(addrstr)),
				 client_port(&addr));
			close(fd);
			free(client);
			return;
		}
	}
#endif

	struct epoll_event ev = {
		.events = EPOLLIN,
		.data.fd = fd,
	};
	if (epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
		RSS_ERROR("epoll_ctl add client fd: %s", strerror(errno));
#ifdef COMPY_HAS_TLS
		if (client->tls)
			Compy_TlsConn_free(client->tls);
#endif
		close(fd);
		free(client);
		return;
	}

	/* Initialize send queue and thread before taking the lock —
	 * readers block on clients_lock so keep the critical section short. */
	pthread_mutex_init(&client->write_lock, NULL);
	if (rsd_sendq_init(&client->sendq) != 0) {
		pthread_mutex_destroy(&client->write_lock);
		goto reject_client;
	}
	{
		pthread_attr_t sa;
		pthread_attr_init(&sa);
		pthread_attr_setstacksize(&sa, 128 * 1024);
		int cret = pthread_create(&client->send_tid, &sa, rsd_client_send_thread, client);
		pthread_attr_destroy(&sa);
		if (cret != 0) {
			rsd_sendq_destroy(&client->sendq);
			pthread_mutex_destroy(&client->write_lock);
			goto reject_client;
		}
		client->send_thread_running = true;
	}

	/* Lock only for capacity check + insert */
	pthread_mutex_lock(&srv->clients_lock);
	if (srv->client_count >= srv->max_clients) {
		pthread_mutex_unlock(&srv->clients_lock);
		RSS_WARN("max clients reached (%d), rejecting", srv->max_clients);
		pthread_mutex_lock(&client->sendq.lock);
		client->sendq.shutdown = true;
		pthread_cond_signal(&client->sendq.cond);
		pthread_mutex_unlock(&client->sendq.lock);
		pthread_join(client->send_tid, NULL);
		rsd_sendq_destroy(&client->sendq);
		pthread_mutex_destroy(&client->write_lock);
		goto reject_client;
	}
	srv->clients[srv->client_count++] = client;
	pthread_mutex_unlock(&srv->clients_lock);
	goto accept_done;

reject_client:
	epoll_ctl(srv->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
#ifdef COMPY_HAS_TLS
	if (client->tls)
		Compy_TlsConn_free(client->tls);
#endif
	close(fd);
	free(client);
	return;

accept_done:;

	char addrstr[INET6_ADDRSTRLEN];
	RSS_INFO("client %s:%u connected%s (%d/%d)",
		 client_addr_str(&addr, addrstr, sizeof(addrstr)), client_port(&addr),
#ifdef COMPY_HAS_TLS
		 client->tls ? " (TLS)" : "",
#else
		 "",
#endif
		 srv->client_count, srv->max_clients);
}

static void handle_client_data(rsd_server_t *srv, int fd)
{
	rsd_client_t *client = find_client_by_fd(srv, fd);
	if (!client)
		return;

	/* Read into client's buffer */
	size_t avail = sizeof(client->recv_buf) - client->recv_len;
	if (avail == 0) {
		RSS_WARN("client recv buffer full, disconnecting");
		remove_client(srv, client);
		return;
	}

	ssize_t n;
#ifdef COMPY_HAS_TLS
	if (client->tls)
		n = compy_tls_read(client->tls, client->recv_buf + client->recv_len, avail);
	else
#endif
		n = read(fd, client->recv_buf + client->recv_len, avail);
	if (n <= 0) {
		remove_client(srv, client);
		return;
	}

	client->recv_len += (size_t)n;
	client->last_activity = rss_timestamp_us();
	rsd_handle_rtsp_data(client, client->recv_buf, client->recv_len);
}

int rsd_server_init(rsd_server_t *srv)
{
	/* Ring names for multi-sensor: sensor 0 = main/sub, sensor N = sN_main/sN_sub */
	static const char *ring_names[RSD_STREAM_COUNT] = {"main",   "sub",	"s1_main",
							   "s1_sub", "s2_main", "s2_sub"};

	/* Try to open audio ring first (fast — no waiting) */
	srv->ring_audio = rss_ring_open("audio");
	if (srv->ring_audio) {
		srv->has_audio = true;
		rss_ring_check_version(srv->ring_audio, "audio");
		RSS_INFO("audio ring available");
	}

	/* Try to open video rings. Wait up to 10s for the main ring only
	 * if no audio is available (need at least one ring to serve). */
	srv->video[RSD_STREAM_MAIN].ring = rss_ring_open("main");
	if (!srv->video[RSD_STREAM_MAIN].ring && !srv->has_audio) {
		RSS_INFO("waiting for video ring...");
		for (int i = 0; i < 100; i++) {
			usleep(100000);
			srv->video[RSD_STREAM_MAIN].ring = rss_ring_open("main");
			if (srv->video[RSD_STREAM_MAIN].ring)
				break;
		}
	}
	if (srv->video[RSD_STREAM_MAIN].ring) {
		rss_ring_check_version(srv->video[RSD_STREAM_MAIN].ring, "main");
		RSS_INFO("stream 0 (main) ring available");
	} else {
		RSS_WARN("main video ring not available (is RVD running?)");
	}
	srv->video[RSD_STREAM_MAIN].idx = RSD_STREAM_MAIN;
	srv->video[RSD_STREAM_MAIN].ring_name = ring_names[RSD_STREAM_MAIN];

	/* Try to open remaining video rings (all optional) */
	for (int s = 1; s < RSD_STREAM_COUNT; s++) {
		srv->video[s].ring = rss_ring_open(ring_names[s]);
		srv->video[s].idx = s;
		srv->video[s].ring_name = ring_names[s];
		if (srv->video[s].ring)
			RSS_INFO("stream %d (%s) ring available", s, ring_names[s]);
	}

	/* Must have at least one ring to serve */
	bool has_any_video = false;
	for (int s = 0; s < RSD_STREAM_COUNT; s++) {
		if (srv->video[s].ring) {
			has_any_video = true;
			break;
		}
	}
	if (!has_any_video && !srv->has_audio) {
		RSS_FATAL("no video or audio rings available");
		return -1;
	}

	/* Log and allocate frame buffers for available video rings */
	for (int s = 0; s < RSD_STREAM_COUNT; s++) {
		if (!srv->video[s].ring)
			continue;

		const rss_ring_header_t *hdr = rss_ring_get_header(srv->video[s].ring);
		if (!hdr || hdr->slot_count == 0) {
			RSS_ERROR("ring[%d]: invalid header (slot_count=0), skipping", s);
			rss_ring_close(srv->video[s].ring);
			srv->video[s].ring = NULL;
			continue;
		}
		RSS_INFO("ring[%d]: %s %ux%u @ %u/%u fps", s, hdr->codec == 0 ? "H.264" : "H.265",
			 hdr->width, hdr->height, hdr->fps_num, hdr->fps_den);
		srv->video[s].last_codec = hdr->codec;
		srv->video[s].last_width = hdr->width;
		srv->video[s].last_height = hdr->height;
		srv->video[s].last_fps_num = hdr->fps_num;
		srv->video[s].last_fps_den = hdr->fps_den;
		srv->video[s].last_profile = hdr->profile;
		srv->video[s].last_level = hdr->level;

		srv->video[s].frame_buf_size = rss_ring_max_frame_size(srv->video[s].ring);
		if (srv->video[s].frame_buf_size < 256 * 1024)
			srv->video[s].frame_buf_size = 256 * 1024;
		srv->video[s].frame_buf = malloc(srv->video[s].frame_buf_size);
		if (!srv->video[s].frame_buf) {
			RSS_FATAL("failed to allocate frame buffer for stream %d", s);
			return -1;
		}
	}

	/* Create dual-stack IPv6 listen socket */
	srv->listen_fd = socket(AF_INET6, SOCK_STREAM, 0);
	if (srv->listen_fd < 0) {
		RSS_FATAL("socket failed: %s", strerror(errno));
		return -1;
	}

	int one = 1;
	setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	/* Dual-stack: accept both IPv4 and IPv6 */
	int zero = 0;
	setsockopt(srv->listen_fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));

	rss_set_nonblocking(srv->listen_fd);

	struct sockaddr_in6 addr = {
		.sin6_family = AF_INET6,
		.sin6_port = htons(srv->port),
		.sin6_addr = in6addr_any,
	};

	if (bind(srv->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		RSS_FATAL("bind port %d failed: %s", srv->port, strerror(errno));
		close(srv->listen_fd);
		return -1;
	}

	if (listen(srv->listen_fd, 4) < 0) {
		RSS_FATAL("listen failed: %s", strerror(errno));
		close(srv->listen_fd);
		return -1;
	}

	/* Epoll */
	srv->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	if (srv->epoll_fd < 0) {
		RSS_FATAL("epoll_create1 failed");
		close(srv->listen_fd);
		return -1;
	}

	struct epoll_event ev = {.events = EPOLLIN, .data.fd = srv->listen_fd};
	if (epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, srv->listen_fd, &ev) < 0)
		RSS_ERROR("epoll_ctl add listen_fd: %s", strerror(errno));

	/* Control socket */
	rss_mkdir_p(RSS_RUN_DIR);
	srv->ctrl = rss_ctrl_listen(RSS_RUN_DIR "/rsd.sock");

	if (srv->ctrl) {
		int ctrl_fd = rss_ctrl_get_fd(srv->ctrl);
		if (ctrl_fd >= 0) {
			ev = (struct epoll_event){.events = EPOLLIN, .data.fd = ctrl_fd};
			if (epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, ctrl_fd, &ev) < 0)
				RSS_ERROR("epoll_ctl add ctrl_fd: %s", strerror(errno));
		}
	}

	const char *proto = "RTSP";
#ifdef COMPY_HAS_TLS
	if (srv->tls_ctx)
		proto = "RTSPS";
#endif
	RSS_INFO("%s server listening on port %d (dual-stack)", proto, srv->port);
	return 0;
}

static int rsd_ctrl_handler(const char *cmd_json, char *resp_buf, int resp_buf_size, void *userdata)
{
	rsd_server_t *srv = userdata;

	int rc = rss_ctrl_handle_common(cmd_json, resp_buf, resp_buf_size, srv->cfg,
					srv->config_path);
	if (rc >= 0)
		return rc;

	char cmd[64];
	if (rss_json_get_str(cmd_json, "cmd", cmd, sizeof(cmd)) != 0)
		return rss_ctrl_resp_error(resp_buf, resp_buf_size, "missing cmd");

	if (strcmp(cmd, "config-show") == 0) {
		cJSON *r = cJSON_CreateObject();
		cJSON_AddStringToObject(r, "status", "ok");
		cJSON *cfg = cJSON_AddObjectToObject(r, "config");
		cJSON_AddNumberToObject(cfg, "port", srv->port);
		cJSON_AddNumberToObject(cfg, "clients", srv->client_count);
		cJSON_AddNumberToObject(cfg, "max_clients", srv->max_clients);
#ifdef COMPY_HAS_TLS
		cJSON_AddBoolToObject(cfg, "tls", srv->tls_ctx != NULL);
#else
		cJSON_AddBoolToObject(cfg, "tls", false);
#endif
		cJSON_AddStringToObject(cfg, "config_path", srv->config_path);
		return rss_ctrl_resp_json(resp_buf, resp_buf_size, r);
	}

	if (strcmp(cmd, "clients") == 0) {
		cJSON *r = cJSON_CreateObject();
		cJSON_AddStringToObject(r, "status", "ok");
		cJSON *arr = cJSON_AddArrayToObject(r, "clients");
		pthread_mutex_lock(&srv->clients_lock);
		cJSON_AddNumberToObject(r, "count", srv->client_count);
		for (int i = 0; i < srv->client_count; i++) {
			rsd_client_t *c = srv->clients[i];
			char addr[INET6_ADDRSTRLEN];
			client_addr_str(&c->addr, addr, sizeof(addr));
			cJSON *item = cJSON_CreateObject();
			cJSON_AddStringToObject(item, "ip", addr);
			cJSON_AddNumberToObject(item, "port", client_port(&c->addr));
			cJSON_AddNumberToObject(item, "stream", c->stream_idx);
			cJSON_AddStringToObject(item, "transport", c->is_tcp ? "tcp" : "udp");
			cJSON_AddBoolToObject(item, "video", c->video.playing);
			cJSON_AddBoolToObject(item, "audio", c->audio.playing);
			cJSON_AddBoolToObject(item, "backchannel", c->backchannel != NULL);
			/* Include RTCP RR stats if available */
			const Compy_RtcpReportBlock *rr = NULL;
			if (c->video.rtcp)
				rr = Compy_Rtcp_get_last_rr(c->video.rtcp);
			if (rr) {
				cJSON_AddNumberToObject(item, "loss_pct",
							rr->fraction_lost * 100.0 / 256.0);
				cJSON_AddNumberToObject(item, "jitter", rr->interarrival_jitter);
				cJSON_AddNumberToObject(item, "cum_lost", rr->cumulative_lost);
			}
			cJSON_AddItemToArray(arr, item);
		}
		pthread_mutex_unlock(&srv->clients_lock);
		return rss_ctrl_resp_json(resp_buf, resp_buf_size, r);
	}

	/* Default: status */
	cJSON *r = cJSON_CreateObject();
	cJSON_AddStringToObject(r, "status", "ok");
	cJSON_AddNumberToObject(r, "clients", srv->client_count);
	cJSON_AddNumberToObject(r, "port", srv->port);
#ifdef COMPY_HAS_TLS
	cJSON_AddBoolToObject(r, "tls", srv->tls_ctx != NULL);
#else
	cJSON_AddBoolToObject(r, "tls", false);
#endif
	return rss_ctrl_resp_json(resp_buf, resp_buf_size, r);
}

void rsd_server_run(rsd_server_t *srv)
{
	/* Start ring reader threads — set back-pointers before spawn */
	for (int s = 0; s < RSD_STREAM_COUNT; s++)
		srv->video[s].srv = srv;
	pthread_t video_tid[RSD_STREAM_COUNT], audio_tid;
	bool video_started[RSD_STREAM_COUNT] = {false};
	bool audio_started = false;

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 128 * 1024);

	for (int s = 0; s < RSD_STREAM_COUNT; s++) {
		if (srv->video[s].ring) {
			pthread_create(&video_tid[s], &attr, rsd_video_reader_thread,
				       &srv->video[s]);
			video_started[s] = true;
		}
	}
	if (srv->has_audio) {
		if (pthread_create(&audio_tid, &attr, rsd_audio_reader_thread, srv) == 0)
			audio_started = true;
		else
			RSS_ERROR("audio reader thread create failed");
	}
	pthread_attr_destroy(&attr);

	struct epoll_event events[16];
	int ctrl_fd = srv->ctrl ? rss_ctrl_get_fd(srv->ctrl) : -1;
	int audio_retry_count = 0;

	while (rss_running(srv->running)) {
		int n = epoll_wait(srv->epoll_fd, events, 16, 500);
		for (int i = 0; i < n; i++) {
			int fd = events[i].data.fd;

			if (fd == srv->listen_fd) {
				accept_client(srv);
			} else if (fd == ctrl_fd) {
				rss_ctrl_accept_and_handle(srv->ctrl, rsd_ctrl_handler, srv);
			} else {
				/* Check if this is a UDP RTCP fd */
				rsd_client_t *rc = find_client_by_rtcp_fd(srv, fd);
				if (rc) {
					uint8_t rtcp_buf[512];
					ssize_t rn = recv(fd, rtcp_buf, sizeof(rtcp_buf), 0);
					if (rn > 0 && rc->video.rtcp)
						Compy_Rtcp_handle_incoming(rc->video.rtcp, rtcp_buf,
									   rn);
				} else {
					handle_client_data(srv, fd);
				}
			}
		}

		/* Register any new UDP RTCP fds with epoll (created by SETUP) */
		for (int i = 0; i < srv->client_count; i++) {
			rsd_client_t *c = srv->clients[i];
			if (c && c->udp_rtcp_fd >= 0 && !c->rtcp_in_epoll) {
				fcntl(c->udp_rtcp_fd, F_SETFL,
				      fcntl(c->udp_rtcp_fd, F_GETFL) | O_NONBLOCK);
				struct epoll_event rtcp_ev = {.events = EPOLLIN,
							      .data.fd = c->udp_rtcp_fd};
				if (epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, c->udp_rtcp_fd,
					      &rtcp_ev) == 0)
					c->rtcp_in_epoll = true;
			}
		}

		/* Idle timeout sweep — disconnect clients with no activity */
		int64_t now = rss_timestamp_us();
		int64_t timeout_us = (int64_t)srv->session_timeout * 1000000;
		for (int i = srv->client_count - 1; i >= 0; i--) {
			rsd_client_t *c = srv->clients[i];
			if (c && !c->video.playing && !c->audio.playing &&
			    (now - c->last_activity) > timeout_us) {
				char addr[INET6_ADDRSTRLEN];
				RSS_WARN("idle timeout: %s:%u (%ds)",
					 client_addr_str(&c->addr, addr, sizeof(addr)),
					 client_port(&c->addr), srv->session_timeout);
				remove_client(srv, c);
			}
		}

		/* Lazy audio ring attach — retry every ~2s until found */
		if (!srv->has_audio && ++audio_retry_count >= 4) {
			audio_retry_count = 0;
			srv->ring_audio = rss_ring_open("audio");
			if (srv->ring_audio) {
				pthread_attr_t a_attr;
				pthread_attr_init(&a_attr);
				pthread_attr_setstacksize(&a_attr, 128 * 1024);
				if (pthread_create(&audio_tid, &a_attr, rsd_audio_reader_thread,
						   srv) == 0) {
					srv->has_audio = true;
					audio_started = true;
					RSS_INFO("audio ring attached (late)");
				} else {
					RSS_ERROR("audio reader thread create failed");
					rss_ring_close(srv->ring_audio);
					srv->ring_audio = NULL;
				}
				pthread_attr_destroy(&a_attr);
			}
		}
	}

	for (int s = 0; s < RSD_STREAM_COUNT; s++) {
		if (video_started[s])
			pthread_join(video_tid[s], NULL);
	}
	if (audio_started)
		pthread_join(audio_tid, NULL);
}

void rsd_server_deinit(rsd_server_t *srv)
{
	/* Remove all clients */
	while (srv->client_count > 0)
		remove_client(srv, srv->clients[0]);

	if (srv->ctrl)
		rss_ctrl_destroy(srv->ctrl);

	for (int s = 0; s < RSD_STREAM_COUNT; s++) {
		if (srv->video[s].ring)
			rss_ring_close(srv->video[s].ring);
		free(srv->video[s].frame_buf);
	}
	if (srv->ring_audio)
		rss_ring_close(srv->ring_audio);

	if (srv->epoll_fd >= 0)
		close(srv->epoll_fd);

	if (srv->listen_fd >= 0)
		close(srv->listen_fd);
}

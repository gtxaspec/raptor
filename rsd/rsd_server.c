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

/* Format client address for logging (IPv4-mapped shown as plain IPv4) */
static const char *client_addr_str(const struct sockaddr_storage *ss, char *buf, size_t bufsz)
{
	if (ss->ss_family == AF_INET) {
		inet_ntop(AF_INET, &((const struct sockaddr_in *)ss)->sin_addr, buf, bufsz);
	} else if (ss->ss_family == AF_INET6) {
		const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)ss;
		if (IN6_IS_ADDR_V4MAPPED(&s6->sin6_addr))
			inet_ntop(AF_INET, &s6->sin6_addr.s6_addr[12], buf, bufsz);
		else
			inet_ntop(AF_INET6, &s6->sin6_addr, buf, bufsz);
	} else {
		snprintf(buf, bufsz, "???");
	}
	return buf;
}

static uint16_t client_port(const struct sockaddr_storage *ss)
{
	if (ss->ss_family == AF_INET)
		return ntohs(((const struct sockaddr_in *)ss)->sin_port);
	if (ss->ss_family == AF_INET6)
		return ntohs(((const struct sockaddr_in6 *)ss)->sin6_port);
	return 0;
}

static int set_nonblocking(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0)
		return -1;
	return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static rsd_client_t *find_client_by_fd(rsd_server_t *srv, int fd)
{
	for (int i = 0; i < srv->client_count; i++) {
		if (srv->clients[i] && srv->clients[i]->fd == fd)
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

	/* Step 2: Now safe to free transports — ring readers won't touch them */

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
	if (client->udp_rtcp_fd >= 0)
		close(client->udp_rtcp_fd);

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

	if (srv->client_count >= RSD_MAX_CLIENTS) {
		RSS_WARN("max clients reached, rejecting");
		close(fd);
		return;
	}

	/* Keep fd blocking for writes — non-blocking would drop RTP packets
	 * when the send buffer fills (78KB keyframes need multiple writes).
	 * Reads are handled by epoll with EPOLLIN. */
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

	/* Increase send buffer for large keyframes */
	int sndbuf = 256 * 1024;
	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

	rsd_client_t *client = calloc(1, sizeof(*client));
	if (!client) {
		close(fd);
		return;
	}
	client->udp_rtp_fd = -1;
	client->udp_rtcp_fd = -1;

	client->fd = fd;
	client->addr = addr;
	client->active = true;
	client->waiting_keyframe = true;

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
	epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, fd, &ev);

	pthread_mutex_lock(&srv->clients_lock);
	srv->clients[srv->client_count++] = client;
	pthread_mutex_unlock(&srv->clients_lock);

	char addrstr[INET6_ADDRSTRLEN];
	RSS_INFO("client %s:%u connected%s (%d/%d)",
		 client_addr_str(&addr, addrstr, sizeof(addrstr)), client_port(&addr),
#ifdef COMPY_HAS_TLS
		 client->tls ? " (TLS)" : "",
#else
		 "",
#endif
		 srv->client_count, RSD_MAX_CLIENTS);
}

static void handle_client_data(rsd_server_t *srv, int fd)
{
	rsd_client_t *client = find_client_by_fd(srv, fd);
	if (!client)
		return;

	/* Read into client's buffer */
	size_t avail = sizeof(client->recv_buf) - client->recv_len;
	if (avail == 0) {
		RSS_WARN("client recv buffer full, dropping");
		client->recv_len = 0;
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
	rsd_handle_rtsp_data(srv, client, client->recv_buf, client->recv_len);
}

int rsd_server_init(rsd_server_t *srv)
{
	/* Wait for main ring to appear */
	RSS_INFO("waiting for video ring...");
	for (int i = 0; i < 100; i++) {
		srv->video[RSD_STREAM_MAIN].ring = rss_ring_open("main");
		if (srv->video[RSD_STREAM_MAIN].ring)
			break;
		usleep(100000);
	}
	if (!srv->video[RSD_STREAM_MAIN].ring) {
		RSS_FATAL("video ring not available (is RVD running?)");
		return -1;
	}
	srv->video[RSD_STREAM_MAIN].idx = RSD_STREAM_MAIN;

	/* Try to open sub ring (optional) */
	srv->video[RSD_STREAM_SUB].ring = rss_ring_open("sub");
	srv->video[RSD_STREAM_SUB].idx = RSD_STREAM_SUB;
	if (srv->video[RSD_STREAM_SUB].ring)
		RSS_INFO("sub stream ring available");

	/* Log and allocate frame buffers for each ring */
	for (int s = 0; s < RSD_STREAM_COUNT; s++) {
		if (!srv->video[s].ring)
			continue;

		const rss_ring_header_t *hdr = rss_ring_get_header(srv->video[s].ring);
		RSS_INFO("ring[%d]: %s %ux%u @ %u/%u fps", s, hdr->codec == 0 ? "H.264" : "H.265",
			 hdr->width, hdr->height, hdr->fps_num, hdr->fps_den);

		/* Frame buffer: sized to largest possible slot payload.
		 * data_size / slot_count = max bytes per frame in the ring. */
		srv->video[s].frame_buf_size = hdr->data_size / hdr->slot_count;
		if (srv->video[s].frame_buf_size < 256 * 1024)
			srv->video[s].frame_buf_size = 256 * 1024;
		srv->video[s].frame_buf = malloc(srv->video[s].frame_buf_size);
		if (!srv->video[s].frame_buf) {
			RSS_FATAL("failed to allocate frame buffer for stream %d", s);
			return -1;
		}
	}

	/* Try to open audio ring (optional — RAD may not be running) */
	srv->ring_audio = rss_ring_open("audio");
	if (srv->ring_audio) {
		srv->has_audio = true;
		RSS_INFO("audio ring available");
	} else {
		RSS_INFO("no audio ring (RAD not running?)");
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

	set_nonblocking(srv->listen_fd);

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
	srv->epoll_fd = epoll_create1(0);
	if (srv->epoll_fd < 0) {
		RSS_FATAL("epoll_create1 failed");
		close(srv->listen_fd);
		return -1;
	}

	struct epoll_event ev = {.events = EPOLLIN, .data.fd = srv->listen_fd};
	epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, srv->listen_fd, &ev);

	/* Control socket */
	rss_mkdir_p("/var/run/rss");
	srv->ctrl = rss_ctrl_listen("/var/run/rss/rsd.sock");

	if (srv->ctrl) {
		int ctrl_fd = rss_ctrl_get_fd(srv->ctrl);
		if (ctrl_fd >= 0) {
			ev = (struct epoll_event){.events = EPOLLIN, .data.fd = ctrl_fd};
			epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, ctrl_fd, &ev);
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

static int json_get_str(const char *json, const char *key, char *buf, int bufsz)
{
	char pattern[64];
	snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
	const char *p = strstr(json, pattern);
	if (!p)
		return -1;
	p += strlen(pattern);
	const char *end = strchr(p, '"');
	if (!end)
		return -1;
	int len = (int)(end - p);
	if (len >= bufsz)
		len = bufsz - 1;
	memcpy(buf, p, len);
	buf[len] = '\0';
	return 0;
}

static int rsd_ctrl_handler(const char *cmd_json, char *resp_buf, int resp_buf_size, void *userdata)
{
	rsd_server_t *srv = userdata;

	if (strstr(cmd_json, "\"config-get\"")) {
		char section[64], key[64];
		if (json_get_str(cmd_json, "section", section, sizeof(section)) == 0 &&
		    json_get_str(cmd_json, "key", key, sizeof(key)) == 0) {
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
		if (ret == 0)
			RSS_INFO("running config saved to %s", srv->config_path);
		return (int)strlen(resp_buf);
	}

	if (strstr(cmd_json, "\"config-show\"")) {
		snprintf(resp_buf, resp_buf_size,
			 "{\"status\":\"ok\",\"config\":{"
			 "\"port\":%d,\"clients\":%d,"
			 "\"max_clients\":%d,"
			 "\"config_path\":\"%s\"}}",
			 srv->port, srv->client_count, RSD_MAX_CLIENTS, srv->config_path);
		return (int)strlen(resp_buf);
	}

	if (strstr(cmd_json, "\"clients\"")) {
		int n = snprintf(resp_buf, resp_buf_size,
				 "{\"status\":\"ok\",\"count\":%d,\"clients\":[",
				 srv->client_count);
		pthread_mutex_lock(&srv->clients_lock);
		for (int i = 0; i < srv->client_count; i++) {
			rsd_client_t *c = srv->clients[i];
			char addr[INET6_ADDRSTRLEN];
			client_addr_str(&c->addr, addr, sizeof(addr));
			n += snprintf(resp_buf + n, resp_buf_size - n,
				      "%s{\"ip\":\"%s\",\"port\":%u,"
				      "\"stream\":%d,\"transport\":\"%s\","
				      "\"video\":%s,\"audio\":%s,\"backchannel\":%s}",
				      i > 0 ? "," : "", addr, client_port(&c->addr), c->stream_idx,
				      c->is_tcp ? "tcp" : "udp",
				      c->video.playing ? "true" : "false",
				      c->audio.playing ? "true" : "false",
				      c->backchannel ? "true" : "false");
		}
		pthread_mutex_unlock(&srv->clients_lock);
		snprintf(resp_buf + n, resp_buf_size - n, "]}");
		return (int)strlen(resp_buf);
	}

	/* Default: status */
	snprintf(resp_buf, resp_buf_size, "{\"status\":\"ok\",\"clients\":%d,\"port\":%d}",
		 srv->client_count, srv->port);
	return (int)strlen(resp_buf);
}

void rsd_server_run(rsd_server_t *srv)
{
	/* Start ring reader threads */
	rsd_set_server_for_readers(srv);
	pthread_t video_tid[RSD_STREAM_COUNT], audio_tid;

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 128 * 1024);

	for (int s = 0; s < RSD_STREAM_COUNT; s++) {
		if (srv->video[s].ring)
			pthread_create(&video_tid[s], &attr, rsd_video_reader_thread,
				       &srv->video[s]);
	}
	if (srv->has_audio)
		pthread_create(&audio_tid, &attr, rsd_audio_reader_thread, srv);
	pthread_attr_destroy(&attr);

	struct epoll_event events[16];
	int ctrl_fd = srv->ctrl ? rss_ctrl_get_fd(srv->ctrl) : -1;

	while (*srv->running) {
		int n = epoll_wait(srv->epoll_fd, events, 16, 500);
		for (int i = 0; i < n; i++) {
			int fd = events[i].data.fd;

			if (fd == srv->listen_fd) {
				accept_client(srv);
			} else if (fd == ctrl_fd) {
				rss_ctrl_accept_and_handle(srv->ctrl, rsd_ctrl_handler, srv);
			} else {
				handle_client_data(srv, fd);
			}
		}
	}

	for (int s = 0; s < RSD_STREAM_COUNT; s++) {
		if (srv->video[s].ring)
			pthread_join(video_tid[s], NULL);
	}
	if (srv->has_audio)
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

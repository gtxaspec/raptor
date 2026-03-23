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

static int set_nonblocking(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0) return -1;
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
	if (!client) return;

	RSS_INFO("client %s:%d disconnected",
		 inet_ntoa(client->addr.sin_addr),
		 ntohs(client->addr.sin_port));

	/* Send RTCP BYE if playing */
	if (client->video.rtcp && client->video.playing) {
		ssize_t bye_ret = Compy_Rtcp_send_bye(client->video.rtcp);
		(void)bye_ret;
	}

	/* Clean up compy objects.
	 * NalTransport owns its RtpTransport, so dropping the NalTransport
	 * drops the RtpTransport chain too. Rtcp is independent. */
	if (client->video.nal) {
		VCALL(DYN(Compy_NalTransport, Compy_Droppable,
			  client->video.nal), drop);
		client->video.nal = NULL;
		client->video.rtp = NULL;
	}

	if (client->video.rtcp) {
		VCALL(DYN(Compy_Rtcp, Compy_Droppable,
			  client->video.rtcp), drop);
		client->video.rtcp = NULL;
	}

	/* Audio cleanup */
	if (client->audio.rtp) {
		VCALL(DYN(Compy_RtpTransport, Compy_Droppable,
			  client->audio.rtp), drop);
		client->audio.rtp = NULL;
	}
	if (client->audio.rtcp) {
		VCALL(DYN(Compy_Rtcp, Compy_Droppable,
			  client->audio.rtcp), drop);
		client->audio.rtcp = NULL;
	}

	/* Close UDP sockets if used */
	if (client->udp_rtp_fd >= 0)
		close(client->udp_rtp_fd);
	if (client->udp_rtcp_fd >= 0)
		close(client->udp_rtcp_fd);

	/* Remove from epoll */
	epoll_ctl(srv->epoll_fd, EPOLL_CTL_DEL, client->fd, NULL);
	close(client->fd);

	/* Remove from client list */
	pthread_mutex_lock(&srv->clients_lock);
	for (int i = 0; i < srv->client_count; i++) {
		if (srv->clients[i] == client) {
			srv->clients[i] = srv->clients[srv->client_count - 1];
			srv->clients[srv->client_count - 1] = NULL;
			srv->client_count--;
			break;
		}
	}
	pthread_mutex_unlock(&srv->clients_lock);

	free(client);
}

static void accept_client(rsd_server_t *srv)
{
	struct sockaddr_in addr;
	socklen_t addrlen = sizeof(addr);
	int fd = accept(srv->listen_fd, (struct sockaddr *)&addr, &addrlen);
	if (fd < 0) return;

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
	if (!client) { close(fd); return; }
	client->udp_rtp_fd = -1;
	client->udp_rtcp_fd = -1;

	client->fd = fd;
	client->addr = addr;
	client->active = true;
	client->waiting_keyframe = true;

	struct epoll_event ev = {
		.events = EPOLLIN,
		.data.fd = fd,
	};
	epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, fd, &ev);

	pthread_mutex_lock(&srv->clients_lock);
	srv->clients[srv->client_count++] = client;
	pthread_mutex_unlock(&srv->clients_lock);

	RSS_INFO("client %s:%d connected (%d/%d)",
		 inet_ntoa(addr.sin_addr), ntohs(addr.sin_port),
		 srv->client_count, RSD_MAX_CLIENTS);
}

static void handle_client_data(rsd_server_t *srv, int fd)
{
	rsd_client_t *client = find_client_by_fd(srv, fd);
	if (!client) return;

	/* Read into client's buffer */
	size_t avail = sizeof(client->recv_buf) - client->recv_len;
	if (avail == 0) {
		RSS_WARN("client recv buffer full, dropping");
		client->recv_len = 0;
		return;
	}

	ssize_t n = read(fd, client->recv_buf + client->recv_len, avail);
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
		srv->ring_main = rss_ring_open("main");
		if (srv->ring_main) break;
		usleep(100000);
	}
	if (!srv->ring_main) {
		RSS_FATAL("video ring not available (is RVD running?)");
		return -1;
	}

	{
		const rss_ring_header_t *hdr = rss_ring_get_header(srv->ring_main);
		RSS_INFO("ring: %s %ux%u @ %u/%u fps",
			 hdr->codec == 0 ? "H.264" : "H.265",
			 hdr->width, hdr->height,
			 hdr->fps_num, hdr->fps_den);

		/* Allocate frame copy buffer sized to largest possible frame */
		srv->frame_buf_size = hdr->data_size / 2;
		if (srv->frame_buf_size < 256 * 1024)
			srv->frame_buf_size = 256 * 1024;
		srv->frame_buf = malloc(srv->frame_buf_size);
		if (!srv->frame_buf) {
			RSS_FATAL("failed to allocate frame buffer");
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

	/* Create listen socket */
	srv->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (srv->listen_fd < 0) {
		RSS_FATAL("socket failed: %s", strerror(errno));
		return -1;
	}

	int one = 1;
	setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	set_nonblocking(srv->listen_fd);

	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(srv->port),
		.sin_addr.s_addr = INADDR_ANY,
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

	struct epoll_event ev = { .events = EPOLLIN, .data.fd = srv->listen_fd };
	epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, srv->listen_fd, &ev);

	/* Control socket */
	rss_mkdir_p("/var/run/rss");
	srv->ctrl = rss_ctrl_listen("/var/run/rss/rsd.sock");

	if (srv->ctrl) {
		int ctrl_fd = rss_ctrl_get_fd(srv->ctrl);
		if (ctrl_fd >= 0) {
			ev = (struct epoll_event){ .events = EPOLLIN, .data.fd = ctrl_fd };
			epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, ctrl_fd, &ev);
		}
	}

	RSS_INFO("RTSP server listening on port %d", srv->port);
	return 0;
}

static int rsd_ctrl_handler(const char *cmd_json, char *resp_buf,
			    int resp_buf_size, void *userdata)
{
	rsd_server_t *srv = userdata;
	(void)cmd_json;
	snprintf(resp_buf, resp_buf_size,
		 "{\"status\":\"ok\",\"clients\":%d}", srv->client_count);
	return 0;
}

void rsd_server_run(rsd_server_t *srv)
{
	/* Start ring reader threads */
	pthread_t video_tid, audio_tid;
	pthread_create(&video_tid, NULL, rsd_ring_reader_thread, srv);
	if (srv->has_audio)
		pthread_create(&audio_tid, NULL, rsd_audio_reader_thread, srv);

	struct epoll_event events[16];
	int ctrl_fd = srv->ctrl ? rss_ctrl_get_fd(srv->ctrl) : -1;

	while (*srv->running) {
		int n = epoll_wait(srv->epoll_fd, events, 16, 500);
		for (int i = 0; i < n; i++) {
			int fd = events[i].data.fd;

			if (fd == srv->listen_fd) {
				accept_client(srv);
			} else if (fd == ctrl_fd) {
				rss_ctrl_accept_and_handle(srv->ctrl,
					rsd_ctrl_handler, srv);
			} else {
				handle_client_data(srv, fd);
			}
		}
	}

	pthread_join(video_tid, NULL);
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

	if (srv->ring_main)
		rss_ring_close(srv->ring_main);
	if (srv->ring_audio)
		rss_ring_close(srv->ring_audio);

	free(srv->frame_buf);
	srv->frame_buf = NULL;

	if (srv->epoll_fd >= 0)
		close(srv->epoll_fd);

	if (srv->listen_fd >= 0)
		close(srv->listen_fd);
}

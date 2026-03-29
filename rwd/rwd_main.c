/*
 * rwd_main.c -- Raptor WebRTC Daemon entry point
 *
 * Minimal WebRTC daemon using WHIP signaling and ICE-lite.
 * Reads H.264 video + Opus audio from SHM rings, sends to
 * browsers via SRTP over a single multiplexed UDP port.
 *
 * Packet demux (RFC 7983):
 *   [0x00..0x03]  → STUN (ICE)
 *   [0x14..0x15]  → DTLS
 *   [0x80..0xBF]  → RTP/RTCP (SRTP)
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
#include <netinet/in.h>
#include <arpa/inet.h>

#include <mbedtls/md.h>

#include "rwd.h"

/* ── Utility: HMAC-SHA1 via mbedTLS ── */

void rwd_hmac_sha1(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len,
		   uint8_t out[20])
{
	const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
	if (!md) {
		memset(out, 0, 20);
		return;
	}
	mbedtls_md_hmac(md, key, key_len, data, data_len, out);
}

/* ── Utility: CSPRNG via /dev/urandom ── */

int rwd_random_bytes(uint8_t *buf, size_t len)
{
	static int urandom_fd = -1;
	if (urandom_fd < 0)
		urandom_fd = open("/dev/urandom", O_RDONLY);
	if (urandom_fd < 0)
		return -1;
	ssize_t n = read(urandom_fd, buf, len);
	return n == (ssize_t)len ? 0 : -1;
}

/* ── Utility: auto-detect local IP address ── */

int rwd_get_local_ip(char *buf, size_t buflen)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return -1;

	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(53),
	};
	inet_pton(AF_INET, "8.8.8.8", &addr.sin_addr);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}

	struct sockaddr_in local;
	socklen_t len = sizeof(local);
	getsockname(fd, (struct sockaddr *)&local, &len);
	close(fd);

	inet_ntop(AF_INET, &local.sin_addr, buf, buflen);
	return 0;
}

/* ── Utility: generate random ICE credentials ── */

void rwd_generate_ice_credentials(char *ufrag, size_t ufrag_len, char *pwd, size_t pwd_len)
{
	static const char charset[] =
		"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+/";
	uint8_t ufrag_rand[8];
	uint8_t pwd_rand[24];

	if (rwd_random_bytes(ufrag_rand, sizeof(ufrag_rand)) != 0) {
		uint64_t ts = rss_timestamp_us();
		memcpy(ufrag_rand, &ts, sizeof(ufrag_rand));
	}
	if (rwd_random_bytes(pwd_rand, sizeof(pwd_rand)) != 0) {
		static uint32_t pwd_counter;
		uint64_t ts = rss_timestamp_us() ^ (uint64_t)(++pwd_counter);
		memcpy(pwd_rand, &ts, sizeof(ts) < sizeof(pwd_rand) ? sizeof(ts) : sizeof(pwd_rand));
	}

	size_t ulen = ufrag_len - 1 < 8 ? ufrag_len - 1 : 8;
	for (size_t i = 0; i < ulen; i++)
		ufrag[i] = charset[ufrag_rand[i] % (sizeof(charset) - 1)];
	ufrag[ulen] = '\0';

	size_t plen = pwd_len - 1 < 24 ? pwd_len - 1 : 24;
	for (size_t i = 0; i < plen; i++)
		pwd[i] = charset[pwd_rand[i] % (sizeof(charset) - 1)];
	pwd[plen] = '\0';
}

/* ── Create listening UDP socket ── */

static int create_udp_socket(int port)
{
	int fd = socket(AF_INET6, SOCK_DGRAM, 0);
	if (fd < 0) {
		/* Fallback to IPv4 if IPv6 not available */
		fd = socket(AF_INET, SOCK_DGRAM, 0);
		if (fd < 0)
			return -1;

		int reuse = 1;
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

		struct sockaddr_in addr = {
			.sin_family = AF_INET,
			.sin_port = htons(port),
			.sin_addr.s_addr = INADDR_ANY,
		};
		if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
			close(fd);
			return -1;
		}
		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
		return fd;
	}

	int reuse = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
	int off = 0;
	setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));

	struct sockaddr_in6 addr = {
		.sin6_family = AF_INET6,
		.sin6_port = htons(port),
		.sin6_addr = IN6ADDR_ANY_INIT,
	};
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}

	/* Set non-blocking for epoll */
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

	return fd;
}

/* ── Create listening TCP socket for HTTP ── */

static int create_http_socket(int port)
{
	int fd = socket(AF_INET6, SOCK_STREAM, 0);
	if (fd >= 0) {
		int reuse = 1;
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
		int off = 0;
		setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));

		struct sockaddr_in6 addr6 = {
			.sin6_family = AF_INET6,
			.sin6_port = htons(port),
			.sin6_addr = IN6ADDR_ANY_INIT,
		};
		if (bind(fd, (struct sockaddr *)&addr6, sizeof(addr6)) < 0) {
			close(fd);
			fd = -1;
		}
	}

	/* Fallback to IPv4 if IPv6 not available */
	if (fd < 0) {
		fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd < 0)
			return -1;

		int reuse = 1;
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

		struct sockaddr_in addr4 = {
			.sin_family = AF_INET,
			.sin_port = htons(port),
			.sin_addr.s_addr = INADDR_ANY,
		};
		if (bind(fd, (struct sockaddr *)&addr4, sizeof(addr4)) < 0) {
			close(fd);
			return -1;
		}
	}

	/* listen backlog of 8 provides implicit connection limiting */
	if (listen(fd, 8) < 0) {
		close(fd);
		return -1;
	}

	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	return fd;
}

/* ── Find client by source address ── */

static rwd_client_t *find_client_by_addr(rwd_server_t *srv, const struct sockaddr_storage *addr,
					 socklen_t addr_len)
{
	for (int i = 0; i < RWD_MAX_CLIENTS; i++) {
		rwd_client_t *c = srv->clients[i];
		if (!c || !c->ice_verified)
			continue;
		if (c->addr_len == addr_len && memcmp(&c->addr, addr, addr_len) == 0)
			return c;
	}
	return NULL;
}

/* ── Handle incoming UDP packet (demux by first byte) ── */

static void handle_udp_packet(rwd_server_t *srv, const uint8_t *buf, size_t len,
			      const struct sockaddr_storage *from, socklen_t from_len)
{
	if (len < 1)
		return;

	uint8_t first = buf[0];

	/* STUN: 0x00-0x03 */
	if (first <= 0x03) {
		rwd_ice_process(srv, buf, len, from, from_len);
		return;
	}

	/* DTLS: 0x14-0x19 (ChangeCipherSpec, Alert, Handshake, AppData)
	 *
	 * Single-buffer design: each recvfrom returns one UDP datagram = one
	 * DTLS record. The buffer is consumed synchronously by handshake_step
	 * before the next recvfrom. Safe as long as packet processing is not
	 * batched ahead of DTLS handling. */
	if (first >= 0x14 && first <= 0x19) {
		pthread_mutex_lock(&srv->clients_lock);
		rwd_client_t *c = find_client_by_addr(srv, from, from_len);
		if (c && c->dtls_state != RWD_DTLS_FAILED) {
			/* Guard: previous record must be consumed */
			if (c->dtls_buf_len > 0)
				RSS_WARN("DTLS: overwriting unconsumed record (%zu bytes)",
					 c->dtls_buf_len);
			size_t copy = len < sizeof(c->dtls_buf) ? len : sizeof(c->dtls_buf);
			memcpy(c->dtls_buf, buf, copy);
			c->dtls_buf_len = copy;

			/* Drive handshake forward */
			int ret = rwd_dtls_handshake_step(c);
			if (ret == 0 && c->dtls_state == RWD_DTLS_ESTABLISHED && !c->media_ready) {
				/* DTLS just completed — set up SRTP */
				if (rwd_media_setup(c) != 0) {
					RSS_ERROR("failed to set up SRTP for %s", c->session_id);
					c->dtls_state = RWD_DTLS_FAILED;
				}
			}
		}
		pthread_mutex_unlock(&srv->clients_lock);
		return;
	}

	/* RTP/RTCP: 0x80-0xBF */
	if (first >= 0x80 && first <= 0xBF) {
		/* Check for RTCP feedback (PLI/FIR) — the first 8 bytes of
		 * SRTCP are unencrypted, so we can read PT and FMT directly.
		 * PT=206 (PSFB): FMT=1=PLI, FMT=4=FIR → request IDR */
		if (len >= 12) {
			uint8_t pt = buf[1] & 0x7F;
			uint8_t fmt = buf[0] & 0x1F;
			if (pt == 206 && (fmt == 1 || fmt == 4)) {
				pthread_mutex_lock(&srv->clients_lock);
				rwd_client_t *c = find_client_by_addr(srv, from, from_len);
				if (c && c->sending) {
					int si = c->stream_idx;
					if (si >= 0 && si < RWD_STREAM_COUNT &&
					    srv->video_rings[si])
						rss_ring_request_idr(srv->video_rings[si]);
				}
				pthread_mutex_unlock(&srv->clients_lock);
			}
		}
		return;
	}
}

/* ── Client cleanup (remove stale sessions) ── */

static void cleanup_stale_clients(rwd_server_t *srv)
{
	int64_t now = rss_timestamp_us();
	int64_t timeout = 30 * 1000000LL; /* 30s without ICE verification */

	pthread_mutex_lock(&srv->clients_lock);
	for (int i = 0; i < RWD_MAX_CLIENTS; i++) {
		rwd_client_t *c = srv->clients[i];
		if (!c)
			continue;

		/* Remove clients that never completed ICE within timeout */
		if (!c->ice_verified && (now - c->created_at) > timeout) {
			RSS_WARN("removing stale session %s (no ICE)", c->session_id);
			rwd_dtls_client_free(c);
			free(c);
			srv->clients[i] = NULL;
			srv->client_count--;
			continue;
		}

		/* Remove clients with failed DTLS */
		if (c->dtls_state == RWD_DTLS_FAILED) {
			RSS_WARN("removing failed session %s", c->session_id);
			rwd_media_teardown(c);
			rwd_dtls_client_free(c);
			free(c);
			srv->clients[i] = NULL;
			srv->client_count--;
			continue;
		}

		/* RFC 7675 consent freshness: remove clients that stopped
		 * sending STUN binding requests. Browsers send them every
		 * ~5s; we allow 30s of silence before evicting. */
		if (c->sending && c->last_stun_at > 0 &&
		    (now - c->last_stun_at) > 30 * 1000000LL) {
			RSS_WARN("removing session %s (consent expired, no STUN for 30s)",
				 c->session_id);
			rwd_media_teardown(c);
			rwd_dtls_client_free(c);
			free(c);
			srv->clients[i] = NULL;
			srv->client_count--;
		}
	}
	pthread_mutex_unlock(&srv->clients_lock);
}

/* ── Control socket handler (raptorctl) ── */

static int rwd_ctrl_handler(const char *cmd_json, char *resp_buf, int resp_buf_size, void *userdata)
{
	rwd_server_t *srv = userdata;

	if (strstr(cmd_json, "\"clients\"")) {
		int n = snprintf(resp_buf, resp_buf_size,
				 "{\"status\":\"ok\",\"count\":%d,\"max_clients\":%d,\"clients\":[",
				 srv->client_count, srv->max_clients);
		pthread_mutex_lock(&srv->clients_lock);
		int first = 1;
		for (int i = 0; i < RWD_MAX_CLIENTS; i++) {
			rwd_client_t *c = srv->clients[i];
			if (!c)
				continue;
			char addr[INET6_ADDRSTRLEN] = "?";
			if (c->addr.ss_family == AF_INET)
				inet_ntop(AF_INET, &((struct sockaddr_in *)&c->addr)->sin_addr,
					  addr, sizeof(addr));
			else if (c->addr.ss_family == AF_INET6) {
				struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&c->addr;
				if (IN6_IS_ADDR_V4MAPPED(&s6->sin6_addr))
					inet_ntop(AF_INET, &s6->sin6_addr.s6_addr[12], addr,
						  sizeof(addr));
				else
					inet_ntop(AF_INET6, &s6->sin6_addr, addr, sizeof(addr));
			}
			n += snprintf(resp_buf + n, resp_buf_size - n,
				      "%s{\"ip\":\"%s\",\"stream\":%d,\"sending\":%s,"
				      "\"ice\":%s,\"dtls\":\"%s\"}",
				      first ? "" : ",", addr, c->stream_idx,
				      c->sending ? "true" : "false",
				      c->ice_verified ? "true" : "false",
				      c->dtls_state == RWD_DTLS_ESTABLISHED ? "established"
				      : c->dtls_state == RWD_DTLS_HANDSHAKING ? "handshaking"
				      : c->dtls_state == RWD_DTLS_FAILED ? "failed"
								         : "new");
			first = 0;
		}
		pthread_mutex_unlock(&srv->clients_lock);
		snprintf(resp_buf + n, resp_buf_size - n, "]}");
		return (int)strlen(resp_buf);
	}

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

	/* Default: status */
	snprintf(resp_buf, resp_buf_size,
		 "{\"status\":\"ok\",\"clients\":%d,\"udp_port\":%d,\"http_port\":%d}",
		 srv->client_count, srv->udp_port, srv->http_port);
	return (int)strlen(resp_buf);
}

/* ── Main event loop ── */

static void rwd_run(rwd_server_t *srv)
{
	srv->epoll_fd = epoll_create1(0);
	if (srv->epoll_fd < 0) {
		RSS_FATAL("epoll_create1 failed: %s", strerror(errno));
		return;
	}

	/* Register UDP socket */
	struct epoll_event ev = {.events = EPOLLIN, .data.fd = srv->udp_fd};
	if (epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, srv->udp_fd, &ev) < 0)
		RSS_ERROR("epoll_ctl add udp_fd failed: %s", strerror(errno));

	/* Register HTTP socket */
	ev = (struct epoll_event){.events = EPOLLIN, .data.fd = srv->http_fd};
	if (epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, srv->http_fd, &ev) < 0)
		RSS_ERROR("epoll_ctl add http_fd failed: %s", strerror(errno));

	/* Register control socket */
	rss_mkdir_p("/var/run/rss");
	srv->ctrl = rss_ctrl_listen("/var/run/rss/rwd.sock");
	int ctrl_fd = -1;
	if (srv->ctrl) {
		ctrl_fd = rss_ctrl_get_fd(srv->ctrl);
		if (ctrl_fd >= 0) {
			ev = (struct epoll_event){.events = EPOLLIN, .data.fd = ctrl_fd};
			if (epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, ctrl_fd, &ev) < 0)
				RSS_ERROR("epoll_ctl add ctrl_fd failed: %s", strerror(errno));
		}
	}

	/* Start media reader threads */
	pthread_t video_tid, audio_tid;
	pthread_create(&video_tid, NULL, rwd_video_reader_thread, srv);
	pthread_create(&audio_tid, NULL, rwd_audio_reader_thread, srv);

	uint8_t udp_buf[RWD_UDP_BUF_SIZE];
	struct epoll_event events[16];
	int64_t last_cleanup = rss_timestamp_us();

	while (*srv->running) {
		int nfds = epoll_wait(srv->epoll_fd, events, 16, 1000);
		if (nfds < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		for (int i = 0; i < nfds; i++) {
			int fd = events[i].data.fd;

			if (fd == srv->udp_fd) {
				/* Receive UDP packets */
				for (int burst = 0; burst < 32; burst++) {
					struct sockaddr_storage from;
					socklen_t from_len = sizeof(from);
					ssize_t n =
						recvfrom(srv->udp_fd, udp_buf, sizeof(udp_buf), 0,
							 (struct sockaddr *)&from, &from_len);
					if (n <= 0)
						break;
					handle_udp_packet(srv, udp_buf, n, &from, from_len);
				}
			} else if (fd == srv->http_fd) {
				/* Accept HTTP connection */
				struct sockaddr_storage client_addr;
				socklen_t client_len = sizeof(client_addr);
				int client_fd = accept(
					srv->http_fd, (struct sockaddr *)&client_addr, &client_len);
				if (client_fd < 0)
					continue;

				/* Guard against slow/malicious clients */
				struct timeval tv = {.tv_sec = 5};
				setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

				/* Get local address for IP detection */
				struct sockaddr_storage local_addr;
				socklen_t local_len = sizeof(local_addr);
				getsockname(client_fd, (struct sockaddr *)&local_addr, &local_len);

				rwd_signaling_handle(srv, client_fd, &local_addr);
			} else if (fd == ctrl_fd) {
				rss_ctrl_accept_and_handle(srv->ctrl, rwd_ctrl_handler, srv);
			}
		}

		/* Periodic cleanup */
		int64_t now = rss_timestamp_us();
		if (now - last_cleanup > 5000000) {
			cleanup_stale_clients(srv);
			last_cleanup = now;
		}
	}

	/* Shutdown */
	pthread_join(video_tid, NULL);
	pthread_join(audio_tid, NULL);

	/* Cleanup all clients */
	pthread_mutex_lock(&srv->clients_lock);
	for (int i = 0; i < RWD_MAX_CLIENTS; i++) {
		if (srv->clients[i]) {
			rwd_media_teardown(srv->clients[i]);
			rwd_dtls_client_free(srv->clients[i]);
			free(srv->clients[i]);
			srv->clients[i] = NULL;
		}
	}
	srv->client_count = 0;
	pthread_mutex_unlock(&srv->clients_lock);

	close(srv->epoll_fd);
}

/* ── Entry point ── */

int main(int argc, char **argv)
{
	srand(time(NULL));

	rss_daemon_ctx_t dctx;
	int ret = rss_daemon_init(&dctx, "rwd", argc, argv);
	if (ret != 0)
		return ret < 0 ? 1 : 0;

	if (!rss_config_get_bool(dctx.cfg, "webrtc", "enabled", false)) {
		RSS_INFO("WebRTC disabled in config");
		rss_config_free(dctx.cfg);
		rss_daemon_cleanup("rwd");
		return 0;
	}

	rwd_server_t srv = {0};
	srv.udp_fd = -1;
	srv.http_fd = -1;
	srv.cfg = dctx.cfg;
	srv.config_path = dctx.config_path;
	srv.running = dctx.running;
	srv.udp_port = rss_config_get_int(dctx.cfg, "webrtc", "udp_port", 8443);
	srv.http_port = rss_config_get_int(dctx.cfg, "webrtc", "http_port", 8554);
	srv.max_clients = rss_config_get_int(dctx.cfg, "webrtc", "max_clients", 2);
	if (srv.max_clients < 1)
		srv.max_clients = 1;
	if (srv.max_clients > RWD_MAX_CLIENTS)
		srv.max_clients = RWD_MAX_CLIENTS;
	pthread_mutex_init(&srv.clients_lock, NULL);

	/* Initialize CRC32 table before any threads start */
	rwd_crc32_init();

	/* Auto-detect local IP */
	const char *cfg_ip = rss_config_get_str(dctx.cfg, "webrtc", "local_ip", "");
	if (cfg_ip[0]) {
		rss_strlcpy(srv.local_ip, cfg_ip, sizeof(srv.local_ip));
	} else {
		rwd_get_local_ip(srv.local_ip, sizeof(srv.local_ip));
	}
	RSS_INFO("local IP: %s", srv.local_ip);

	/* Initialize DTLS */
	const char *cert_path = rss_config_get_str(dctx.cfg, "webrtc", "cert", "");
	const char *key_path = rss_config_get_str(dctx.cfg, "webrtc", "key", "");
	if (!cert_path[0] || !key_path[0]) {
		RSS_FATAL("WebRTC requires cert and key paths in [webrtc] config");
		goto cleanup;
	}
	srv.dtls = calloc(1, sizeof(*srv.dtls));
	if (!srv.dtls) {
		RSS_FATAL("failed to allocate DTLS context");
		goto cleanup;
	}
	if (rwd_dtls_init(srv.dtls, cert_path, key_path) != 0) {
		RSS_FATAL("DTLS init failed");
		goto cleanup;
	}

	/* Create sockets */
	srv.udp_fd = create_udp_socket(srv.udp_port);
	if (srv.udp_fd < 0) {
		RSS_FATAL("failed to bind UDP port %d: %s", srv.udp_port, strerror(errno));
		goto cleanup;
	}
	RSS_INFO("UDP listening on port %d", srv.udp_port);

	srv.http_fd = create_http_socket(srv.http_port);
	if (srv.http_fd < 0) {
		RSS_FATAL("failed to bind HTTP port %d: %s", srv.http_port, strerror(errno));
		goto cleanup;
	}
	RSS_INFO("HTTP listening on port %d (GET /webrtc, POST /whip)", srv.http_port);

	/* Run */
	rwd_run(&srv);

	RSS_INFO("rwd shutting down");

cleanup:
	if (srv.udp_fd >= 0)
		close(srv.udp_fd);
	if (srv.http_fd >= 0)
		close(srv.http_fd);
	for (int s = 0; s < RWD_STREAM_COUNT; s++) {
		if (srv.video_rings[s])
			rss_ring_close(srv.video_rings[s]);
	}
	if (srv.audio_ring)
		rss_ring_close(srv.audio_ring);
	if (srv.dtls) {
		rwd_dtls_free(srv.dtls);
		free(srv.dtls);
	}
	pthread_mutex_destroy(&srv.clients_lock);
	rss_config_free(dctx.cfg);
	rss_daemon_cleanup("rwd");

	return 0;
}

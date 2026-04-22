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
#include <rss_net.h>
#include <rss_http.h>

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

static int urandom_fd = -1;

void rwd_random_init(void)
{
	if (urandom_fd < 0)
		urandom_fd = open("/dev/urandom", O_RDONLY);
}

int rwd_random_bytes(uint8_t *buf, size_t len)
{
	if (urandom_fd < 0)
		return -1;
	ssize_t n = read(urandom_fd, buf, len);
	return n == (ssize_t)len ? 0 : -1;
}

/* ── Utility: auto-detect local IP address ── */

int rwd_get_local_ip(char *buf, size_t buflen)
{
	/* Try IPv6 first (connect to Google DNS to discover local address) */
	int fd = socket(AF_INET6, SOCK_DGRAM, 0);
	if (fd >= 0) {
		struct sockaddr_in6 addr6 = {
			.sin6_family = AF_INET6,
			.sin6_port = htons(53),
		};
		inet_pton(AF_INET6, "2001:4860:4860::8888", &addr6.sin6_addr);
		if (connect(fd, (struct sockaddr *)&addr6, sizeof(addr6)) == 0) {
			struct sockaddr_in6 local;
			socklen_t len = sizeof(local);
			getsockname(fd, (struct sockaddr *)&local, &len);
			close(fd);
			inet_ntop(AF_INET6, &local.sin6_addr, buf, buflen);
			return 0;
		}
		close(fd);
	}

	/* Fallback: IPv4 */
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd >= 0) {
		struct sockaddr_in addr = {
			.sin_family = AF_INET,
			.sin_port = htons(53),
		};
		inet_pton(AF_INET, "8.8.8.8", &addr.sin_addr);
		if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
			struct sockaddr_in local;
			socklen_t len = sizeof(local);
			getsockname(fd, (struct sockaddr *)&local, &len);
			close(fd);
			inet_ntop(AF_INET, &local.sin_addr, buf, buflen);
			return 0;
		}
		close(fd);
	}

	return -1;
}

/* ── Utility: generate random ICE credentials ── */

void rwd_generate_ice_credentials(char *ufrag, size_t ufrag_len, char *pwd, size_t pwd_len)
{
	static const char charset[] =
		"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+/";
	uint8_t ufrag_rand[8];
	uint8_t pwd_rand[24];

	/* ICE credentials must be cryptographically random — predictable
	 * values allow an attacker to forge STUN HMAC and hijack sessions.
	 * Fail closed: zero-fill produces obviously invalid credentials
	 * that will fail HMAC verification on both sides. */
	memset(ufrag_rand, 0, sizeof(ufrag_rand));
	memset(pwd_rand, 0, sizeof(pwd_rand));

	if (rwd_random_bytes(ufrag_rand, sizeof(ufrag_rand)) != 0)
		RSS_ERROR("ICE: CSPRNG failed for ufrag — session will fail");
	if (rwd_random_bytes(pwd_rand, sizeof(pwd_rand)) != 0)
		RSS_ERROR("ICE: CSPRNG failed for pwd — session will fail");

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

/* Compare sockaddr by family-specific fields only (not padding bytes).
 * memcmp on sockaddr_storage can fail on uninitialized padding. */
static bool sockaddr_equal(const struct sockaddr_storage *a, const struct sockaddr_storage *b)
{
	if (a->ss_family != b->ss_family)
		return false;
	if (a->ss_family == AF_INET) {
		const struct sockaddr_in *a4 = (const struct sockaddr_in *)a;
		const struct sockaddr_in *b4 = (const struct sockaddr_in *)b;
		return a4->sin_port == b4->sin_port && a4->sin_addr.s_addr == b4->sin_addr.s_addr;
	}
	if (a->ss_family == AF_INET6) {
		const struct sockaddr_in6 *a6 = (const struct sockaddr_in6 *)a;
		const struct sockaddr_in6 *b6 = (const struct sockaddr_in6 *)b;
		return a6->sin6_port == b6->sin6_port &&
		       memcmp(&a6->sin6_addr, &b6->sin6_addr, 16) == 0;
	}
	return false;
}

static rwd_client_t *find_client_by_addr(rwd_server_t *srv, const struct sockaddr_storage *addr,
					 socklen_t addr_len)
{
	(void)addr_len;
	for (int i = 0; i < RWD_MAX_CLIENTS; i++) {
		rwd_client_t *c = srv->clients[i];
		if (!c || !c->ice_verified)
			continue;
		if (sockaddr_equal(&c->addr, addr))
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
		if (len < 12)
			return;

		/* Check for RTCP feedback (PLI/FIR) — the first 8 bytes of
		 * SRTCP are unencrypted, so we can read PT and FMT directly.
		 * PT=206 (PSFB): FMT=1=PLI, FMT=4=FIR → request IDR.
		 *
		 * Security note: this acts on unverified SRTCP headers. An
		 * attacker spoofing the ICE-verified source address could
		 * trigger repeated IDR requests (bandwidth/CPU amplification).
		 * Mitigated by: source must match ICE-verified addr, client
		 * must be in sending state, and rwd_request_idr enforces
		 * a per-stream cooldown. */
		uint8_t pt = buf[1] & 0x7F;
		uint8_t fmt = buf[0] & 0x1F;
		if (pt == 206 && (fmt == 1 || fmt == 4)) {
			pthread_mutex_lock(&srv->clients_lock);
			rwd_client_t *c = find_client_by_addr(srv, from, from_len);
			if (c && c->sending)
				rwd_request_idr(srv, c->stream_idx);
			pthread_mutex_unlock(&srv->clients_lock);
		}

		/* Feed all incoming RTP/RTCP to backchannel receiver.
		 * Copy to mutable buffer — SRTP decrypt is in-place. */
		pthread_mutex_lock(&srv->clients_lock);
		rwd_client_t *c = find_client_by_addr(srv, from, from_len);
		if (c && c->srtp_recv) {
			uint8_t rtp_buf[RWD_UDP_BUF_SIZE];
			if (len <= sizeof(rtp_buf)) {
				memcpy(rtp_buf, buf, len);
				rwd_media_feed_rtp(c, rtp_buf, len);
			}
		}
		pthread_mutex_unlock(&srv->clients_lock);
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
		if (c->sending && c->last_stun_at > 0 && (now - c->last_stun_at) > 30 * 1000000LL) {
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

	int rc = rss_ctrl_handle_common(cmd_json, resp_buf, resp_buf_size, srv->cfg,
					srv->config_path);
	if (rc >= 0)
		return rc;

	char cmd[64];
	if (rss_json_get_str(cmd_json, "cmd", cmd, sizeof(cmd)) != 0)
		return rss_ctrl_resp_error(resp_buf, resp_buf_size, "missing cmd");

	if (strcmp(cmd, "clients") == 0) {
		cJSON *r = cJSON_CreateObject();
		cJSON_AddStringToObject(r, "status", "ok");
		cJSON_AddNumberToObject(r, "max_clients", srv->max_clients);
		cJSON *arr = cJSON_AddArrayToObject(r, "clients");
		pthread_mutex_lock(&srv->clients_lock);
		cJSON_AddNumberToObject(r, "count", srv->client_count);
		for (int i = 0; i < RWD_MAX_CLIENTS; i++) {
			rwd_client_t *c = srv->clients[i];
			if (!c)
				continue;
			char addr[INET6_ADDRSTRLEN];
			rss_addr_str(&c->addr, addr, sizeof(addr));
			cJSON *item = cJSON_CreateObject();
			cJSON_AddStringToObject(item, "ip", addr);
			cJSON_AddNumberToObject(item, "stream", c->stream_idx);
			cJSON_AddBoolToObject(item, "sending", c->sending);
			cJSON_AddBoolToObject(item, "ice", c->ice_verified);
			cJSON_AddStringToObject(
				item, "dtls",
				c->dtls_state == RWD_DTLS_ESTABLISHED	? "established"
				: c->dtls_state == RWD_DTLS_HANDSHAKING ? "handshaking"
				: c->dtls_state == RWD_DTLS_FAILED	? "failed"
									: "new");
			cJSON_AddItemToArray(arr, item);
		}
		pthread_mutex_unlock(&srv->clients_lock);
		return rss_ctrl_resp_json(resp_buf, resp_buf_size, r);
	}

	if (strcmp(cmd, "share-rotate") == 0) {
		if (srv->webtorrent) {
			rwd_webtorrent_t *wt = srv->webtorrent;
			rwd_webtorrent_rotate_key(wt);
			cJSON *r = cJSON_CreateObject();
			cJSON_AddStringToObject(r, "status", "ok");
			cJSON_AddStringToObject(r, "key", wt->share_key);
			char url[512];
			snprintf(url, sizeof(url), "%s#key=%s", wt->viewer_base_url, wt->share_key);
			cJSON_AddStringToObject(r, "url", url);
			return rss_ctrl_resp_json(resp_buf, resp_buf_size, r);
		}
		return rss_ctrl_resp_error(resp_buf, resp_buf_size, "webtorrent not enabled");
	}

	if (strcmp(cmd, "share") == 0) {
		if (srv->webtorrent) {
			rwd_webtorrent_t *wt = srv->webtorrent;
			cJSON *r = cJSON_CreateObject();
			cJSON_AddStringToObject(r, "status", "ok");
			cJSON_AddStringToObject(r, "key", wt->share_key);
			char url[512];
			snprintf(url, sizeof(url), "%s#key=%s", wt->viewer_base_url, wt->share_key);
			cJSON_AddStringToObject(r, "url", url);
			return rss_ctrl_resp_json(resp_buf, resp_buf_size, r);
		}
		return rss_ctrl_resp_error(resp_buf, resp_buf_size, "webtorrent not enabled");
	}

	/* Default: status */
	{
		cJSON *r = cJSON_CreateObject();
		cJSON_AddStringToObject(r, "status", "ok");
		cJSON_AddNumberToObject(r, "clients", srv->client_count);
		cJSON_AddNumberToObject(r, "udp_port", srv->udp_port);
		cJSON_AddNumberToObject(r, "http_port", srv->http_port);
		cJSON_AddBoolToObject(r, "dtls", srv->dtls != NULL);
#ifdef RSS_HAS_TLS
		cJSON_AddBoolToObject(r, "https", srv->tls != NULL);
#else
		cJSON_AddBoolToObject(r, "https", false);
#endif
		return rss_ctrl_resp_json(resp_buf, resp_buf_size, r);
	}
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
	rss_mkdir_p(RSS_RUN_DIR);
	srv->ctrl = rss_ctrl_listen(RSS_RUN_DIR "/rwd.sock");
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
	pthread_attr_t tattr;
	pthread_attr_init(&tattr);
	pthread_attr_setstacksize(&tattr, 128 * 1024);

	pthread_t video_tid, audio_tid;
	pthread_create(&video_tid, &tattr, rwd_video_reader_thread, srv);
	pthread_create(&audio_tid, &tattr, rwd_audio_reader_thread, srv);
	pthread_attr_destroy(&tattr);

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

				/* Non-blocking socket — the read loop in
				 * rwd_signaling_handle still retries for up to
				 * ~250ms (50 × 5ms) plus TLS handshake time.
				 * Blocks the main loop during signaling, which
				 * is acceptable for ≤4 WebRTC clients. */
				fcntl(client_fd, F_SETFL, fcntl(client_fd, F_GETFL) | O_NONBLOCK);

				char addrstr[INET6_ADDRSTRLEN];
				RSS_DEBUG("http: %s:%u connected",
					  rss_addr_str(&client_addr, addrstr, sizeof(addrstr)),
					  rss_addr_port(&client_addr));

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

	/* Shutdown — wake reader threads so they see *running == false */
	pthread_cond_broadcast(&srv->clients_cond);
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
	rss_daemon_ctx_t dctx;
	int ret = rss_daemon_init(&dctx, "rwd", argc, argv,
				  ""
#ifdef RAPTOR_WEBTORRENT
				  " webtorrent"
#endif
#ifdef RAPTOR_AAC
				  " aac"
#endif
	);
	if (ret != 0)
		return ret < 0 ? 1 : 0;
	if (!rss_config_get_bool(dctx.cfg, "webrtc", "enabled", false)) {
		RSS_INFO("WebRTC disabled in config");
		rss_config_free(dctx.cfg);
		rss_daemon_cleanup("rwd");
		return 0;
	}

	rwd_random_init();

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
	const char *audio_mode_str = rss_config_get_str(dctx.cfg, "webrtc", "audio_mode", "auto");
	srv.audio_mode =
		(strcmp(audio_mode_str, "opus") == 0) ? RWD_AUDIO_MODE_OPUS : RWD_AUDIO_MODE_AUTO;
	srv.opus_complexity = rss_config_get_int(dctx.cfg, "webrtc", "opus_complexity", 2);
	if (srv.opus_complexity < 0)
		srv.opus_complexity = 0;
	if (srv.opus_complexity > 10)
		srv.opus_complexity = 10;
	srv.opus_bitrate = rss_config_get_int(dctx.cfg, "webrtc", "opus_bitrate", 64000);
	/* Defaults until audio reader detects ring codec */
	srv.wire_codec = RWD_CODEC_OPUS;
	srv.wire_pt = RWD_AUDIO_PT;
	srv.wire_clock = RWD_AUDIO_CLOCK;
	pthread_mutex_init(&srv.clients_lock, NULL);
	pthread_cond_init(&srv.clients_cond, NULL);

	/* Basic auth — enabled when both username and password are set */
	const char *webrtc_user = rss_config_get_str(dctx.cfg, "webrtc", "username", "");
	const char *webrtc_pass = rss_config_get_str(dctx.cfg, "webrtc", "password", "");
	if (webrtc_user[0] && webrtc_pass[0]) {
		rss_base64_init();
		rss_strlcpy(srv.auth_user, webrtc_user, sizeof(srv.auth_user));
		rss_strlcpy(srv.auth_pass, webrtc_pass, sizeof(srv.auth_pass));
		RSS_INFO("WebRTC Basic auth enabled");
	}

	/* Initialize CRC32 table before any threads start */
	rwd_crc32_init();

	/* Auto-detect local IP */
	const char *cfg_ip = rss_config_get_str(dctx.cfg, "webrtc", "local_ip", "");
	if (cfg_ip[0]) {
		rss_strlcpy(srv.local_ip, cfg_ip, sizeof(srv.local_ip));
		srv.local_ip_configured = true;
	} else {
		rwd_get_local_ip(srv.local_ip, sizeof(srv.local_ip));
		srv.local_ip_configured = false;
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

#ifdef RSS_HAS_TLS
	/* HTTPS for signaling — reuses the same cert/key as DTLS.
	 * Enabled by default; disable with https = false in [webrtc]. */
	bool https_enabled = rss_config_get_bool(dctx.cfg, "webrtc", "https", true);
	if (https_enabled) {
		srv.tls = rss_tls_init(cert_path, key_path);
		if (srv.tls)
			RSS_INFO("HTTPS enabled for signaling");
		else
			RSS_WARN("HTTPS init failed, falling back to HTTP");
	}
#endif

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

	/* WebTorrent external sharing (optional) */
#ifdef RAPTOR_WEBTORRENT
	rwd_webtorrent_t wt = {0};
	bool wt_started = false;
	if (rss_config_get_bool(dctx.cfg, "webtorrent", "enabled", false)) {
		rss_strlcpy(wt.tracker_url,
			    rss_config_get_str(dctx.cfg, "webtorrent", "tracker",
					       "wss://tracker.openwebtorrent.com"),
			    sizeof(wt.tracker_url));
		rss_strlcpy(wt.stun_server,
			    rss_config_get_str(dctx.cfg, "webtorrent", "stun_server",
					       "stun.l.google.com"),
			    sizeof(wt.stun_server));
		wt.stun_port = rss_config_get_int(dctx.cfg, "webtorrent", "stun_port", 19302);
		wt.tls_verify = rss_config_get_bool(dctx.cfg, "webtorrent", "tls_verify", true);
		rss_strlcpy(wt.viewer_base_url,
			    rss_config_get_str(dctx.cfg, "webtorrent", "viewer_url",
					       "https://viewer.thingino.com/share.html"),
			    sizeof(wt.viewer_base_url));
		if (rwd_webtorrent_start(&wt, &srv) == 0) {
			wt_started = true;
			srv.webtorrent = &wt;
		} else {
			RSS_ERROR("webtorrent: failed to start");
		}
	}
#endif

	/* Run */
	rwd_run(&srv);

	RSS_INFO("rwd shutting down");

#ifdef RAPTOR_WEBTORRENT
	if (wt_started)
		rwd_webtorrent_stop(&wt);
#endif

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
#ifdef RSS_HAS_TLS
	rss_tls_free(srv.tls);
#endif
	rwd_signaling_cleanup();
	pthread_cond_destroy(&srv.clients_cond);
	pthread_mutex_destroy(&srv.clients_lock);
	rss_config_free(dctx.cfg);
	rss_daemon_cleanup("rwd");

	return 0;
}

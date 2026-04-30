/*
 * rsr_srt.c — SRT listener, client management, send
 */

#include "rsr.h"

#include <string.h>
#include <inttypes.h>
#include <arpa/inet.h>

/* ── Helpers ── */

void rsr_client_addr_str(const struct sockaddr_storage *addr, char *buf, size_t buf_size)
{
	if (addr->ss_family == AF_INET) {
		const struct sockaddr_in *a4 = (const struct sockaddr_in *)addr;
		char ip[INET_ADDRSTRLEN];

		inet_ntop(AF_INET, &a4->sin_addr, ip, sizeof(ip));
		snprintf(buf, buf_size, "%s:%d", ip, ntohs(a4->sin_port));
	} else if (addr->ss_family == AF_INET6) {
		const struct sockaddr_in6 *a6 = (const struct sockaddr_in6 *)addr;
		char ip[INET6_ADDRSTRLEN];

		inet_ntop(AF_INET6, &a6->sin6_addr, ip, sizeof(ip));
		snprintf(buf, buf_size, "[%s]:%d", ip, ntohs(a6->sin6_port));
	} else {
		snprintf(buf, buf_size, "unknown");
	}
}

/* ── STREAMID callback (called before accept) ── */

static int listen_callback(void *opaque, SRTSOCKET ns, int hs_version,
			   const struct sockaddr *peeraddr, const char *streamid)
{
	rsr_state_t *st = opaque;

	(void)ns;
	(void)hs_version;
	(void)peeraddr;

	if (st->client_count >= st->max_clients) {
		RSS_WARN("rejecting connection: max clients (%d)", st->max_clients);
		return -1;
	}

	/* Validate STREAMID if provided */
	if (streamid && streamid[0] != '\0') {
		for (int i = 0; i < RSR_MAX_STREAMS; i++) {
			if (strcmp(streamid, rsr_ring_names[i]) == 0)
				return 0;
		}
		RSS_WARN("rejecting connection: unknown streamid '%s'", streamid);
		return -1;
	}

	return 0;
}

/* ── Init ── */

int rsr_srt_init(rsr_state_t *st)
{
	if (srt_startup() != 0) {
		RSS_ERROR("srt_startup failed");
		return -1;
	}

	st->listener = srt_create_socket();
	if (st->listener == SRT_INVALID_SOCK) {
		RSS_ERROR("srt_create_socket: %s", srt_getlasterror_str());
		srt_cleanup();
		return -1;
	}

	int val;

	val = st->latency_ms;
	srt_setsockflag(st->listener, SRTO_LATENCY, &val, sizeof(val));

	val = 0;
	srt_setsockflag(st->listener, SRTO_RCVSYN, &val, sizeof(val));
	srt_setsockflag(st->listener, SRTO_IPV6ONLY, &val, sizeof(val));

	val = 1;
	srt_setsockflag(st->listener, SRTO_SENDER, &val, sizeof(val));

	if (st->passphrase[0] != '\0') {
		srt_setsockflag(st->listener, SRTO_PASSPHRASE, st->passphrase,
				(int)strlen(st->passphrase));
		srt_setsockflag(st->listener, SRTO_PBKEYLEN, &st->pbkeylen, sizeof(st->pbkeylen));
	}

	srt_listen_callback(st->listener, listen_callback, st);

	struct sockaddr_in6 addr;

	memset(&addr, 0, sizeof(addr));
	addr.sin6_family = AF_INET6;
	addr.sin6_port = htons((uint16_t)st->port);
	addr.sin6_addr = in6addr_any;

	if (srt_bind(st->listener, (struct sockaddr *)&addr, sizeof(addr)) == SRT_ERROR) {
		RSS_ERROR("srt_bind port %d: %s", st->port, srt_getlasterror_str());
		srt_close(st->listener);
		srt_cleanup();
		return -1;
	}

	if (srt_listen(st->listener, 5) == SRT_ERROR) {
		RSS_ERROR("srt_listen: %s", srt_getlasterror_str());
		srt_close(st->listener);
		srt_cleanup();
		return -1;
	}

	st->srt_eid = srt_epoll_create();
	if (st->srt_eid < 0) {
		RSS_ERROR("srt_epoll_create: %s", srt_getlasterror_str());
		srt_close(st->listener);
		srt_cleanup();
		return -1;
	}

	int events = SRT_EPOLL_IN;

	srt_epoll_add_usock(st->srt_eid, st->listener, &events);

	RSS_INFO("SRT listener on port %d (latency=%dms, max_clients=%d%s)", st->port,
		 st->latency_ms, st->max_clients, st->passphrase[0] ? ", encrypted" : "");

	return 0;
}

/* ── Accept new client ── */

static void accept_client(rsr_state_t *st)
{
	struct sockaddr_storage peer;
	int addrlen = sizeof(peer);
	SRTSOCKET csock = srt_accept(st->listener, (struct sockaddr *)&peer, &addrlen);

	if (csock == SRT_INVALID_SOCK)
		return;

	if (st->client_count >= st->max_clients) {
		srt_close(csock);
		return;
	}

	/* Read STREAMID to determine which stream */
	char streamid[64] = {0};
	int sid_len = sizeof(streamid);

	srt_getsockflag(csock, SRTO_STREAMID, streamid, &sid_len);

	const char *ring_name;

	if (streamid[0] != '\0')
		ring_name = streamid;
	else
		ring_name = rsr_ring_names[st->default_stream_idx];

	/* Open or reuse stream */
	rsr_stream_t *stream = rsr_stream_get_or_open(st, ring_name);

	if (!stream) {
		RSS_WARN("cannot open ring '%s' for client, rejecting", ring_name);
		srt_close(csock);
		return;
	}

	/* Set non-blocking send on client socket */
	int no = 0;

	srt_setsockflag(csock, SRTO_SNDSYN, &no, sizeof(no));

	/* Add client to epoll for error detection */
	int events = SRT_EPOLL_ERR;

	srt_epoll_add_usock(st->srt_eid, csock, &events);

	rsr_client_t *c = &st->clients[st->client_count];

	memset(c, 0, sizeof(*c));
	c->sock = csock;
	memcpy(&c->addr, &peer, sizeof(peer));
	c->stream = stream;
	c->waiting_keyframe = true;
	c->connect_time_us = rss_timestamp_us();
	stream->client_count++;

	/* Init per-client TS mux */
	uint8_t vtype = stream->codec == 1 ? RSS_TS_STREAM_H265 : RSS_TS_STREAM_H264;

	rss_ts_init(&c->ts, vtype, st->audio_ts_type, 0);

	st->client_count++;

	char addr_str[64];

	rsr_client_addr_str(&peer, addr_str, sizeof(addr_str));
	RSS_INFO("client connected: %s stream=%s (%d/%d)", addr_str, ring_name, st->client_count,
		 st->max_clients);
}

/* ── Remove client ── */

void rsr_remove_client(rsr_state_t *st, int idx)
{
	rsr_client_t *c = &st->clients[idx];
	char addr_str[64];

	rsr_client_addr_str(&c->addr, addr_str, sizeof(addr_str));
	RSS_INFO("client disconnected: %s (sent %" PRIu64 " frames, %" PRIu64 " bytes)", addr_str,
		 c->frames_sent, c->bytes_sent);

	srt_epoll_remove_usock(st->srt_eid, c->sock);
	srt_close(c->sock);

	rsr_stream_t *stream = c->stream;

	stream->client_count--;
	if (stream->client_count <= 0)
		rsr_stream_release(st, stream);

	/* Shift remaining clients down */
	for (int i = idx; i < st->client_count - 1; i++)
		st->clients[i] = st->clients[i + 1];

	st->client_count--;
}

/* ── Poll for events ── */

void rsr_srt_poll(rsr_state_t *st)
{
	SRTSOCKET rfds[RSR_MAX_CLIENTS + 1];
	int rnum = RSR_MAX_CLIENTS + 1;

	if (srt_epoll_wait(st->srt_eid, rfds, &rnum, NULL, NULL, 0, NULL, NULL, NULL, NULL) <= 0)
		return;

	for (int i = 0; i < rnum; i++) {
		if (rfds[i] == st->listener) {
			accept_client(st);
			continue;
		}

		/* Error on a client socket — find and remove */
		for (int j = 0; j < st->client_count; j++) {
			if (st->clients[j].sock == rfds[i]) {
				rsr_remove_client(st, j);
				break;
			}
		}
	}
}

/* ── Send TS data to a single client (chunked 1316-byte sends) ── */

int rsr_srt_send_to_client(rsr_client_t *c, const uint8_t *data, size_t len)
{
	size_t off = 0;

	while (off < len) {
		size_t chunk = len - off;

		if (chunk > RSR_SRT_PAYLOAD_SIZE)
			chunk = RSR_SRT_PAYLOAD_SIZE;

		int ret = srt_send(c->sock, (const char *)(data + off), (int)chunk);

		if (ret == SRT_ERROR) {
			int err = srt_getlasterror(NULL);

			if (err == SRT_EASYNCSND)
				RSS_DEBUG("srt send buffer full, dropping %zu bytes", len - off);
			else
				RSS_DEBUG("srt send error: %s", srt_getlasterror_str());
			return -1;
		}
		off += (size_t)ret;
		c->bytes_sent += (uint64_t)ret;
	}

	return 0;
}

/* ── Cleanup ── */

void rsr_srt_cleanup(rsr_state_t *st)
{
	while (st->client_count > 0)
		rsr_remove_client(st, st->client_count - 1);

	if (st->srt_eid >= 0) {
		srt_epoll_release(st->srt_eid);
		st->srt_eid = -1;
	}

	if (st->listener != SRT_INVALID_SOCK) {
		srt_close(st->listener);
		st->listener = SRT_INVALID_SOCK;
	}

	srt_cleanup();
}

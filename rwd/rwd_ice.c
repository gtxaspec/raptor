/*
 * rwd_ice.c -- ICE-lite STUN handler
 *
 * Camera is always the ICE-lite answerer. Handles STUN binding
 * requests and responds with binding responses. No TURN, no
 * candidate gathering, no trickle ICE. Single host candidate.
 *
 * RFC 5389 (STUN), RFC 8445 (ICE), RFC 8445 Section 2.2 (ICE-lite).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include "rwd.h"

/* ── CRC32 (ISO 3309 / ITU-T V.42) for STUN FINGERPRINT ── */

static uint32_t crc32_table[256];

/* Called from main() before any threads start */
void rwd_crc32_init(void)
{
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i;
		for (int j = 0; j < 8; j++)
			c = (c >> 1) ^ (c & 1 ? 0xEDB88320u : 0);
		crc32_table[i] = c;
	}
}

static uint32_t crc32_compute(const uint8_t *data, size_t len)
{
	uint32_t crc = 0xFFFFFFFF;
	for (size_t i = 0; i < len; i++)
		crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
	return crc ^ 0xFFFFFFFF;
}

/* ── Byte helpers ── */

static inline uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t rd32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static inline void wr16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static inline void wr32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

/* ── STUN MESSAGE-INTEGRITY verification/generation ── */

static int stun_verify_integrity(const uint8_t *msg, size_t msg_len, const char *ice_pwd)
{
	/* Find MESSAGE-INTEGRITY attribute */
	if (msg_len < STUN_HEADER_SIZE + 24)
		return -1;

	const uint8_t *p = msg + STUN_HEADER_SIZE;
	const uint8_t *end = msg + msg_len;
	const uint8_t *mi_attr = NULL;
	size_t mi_offset = 0;

	while (p + 4 <= end) {
		uint16_t attr_type = rd16(p);
		uint16_t attr_len = rd16(p + 2);
		size_t padded = (attr_len + 3) & ~3u;

		if (p + 4 + attr_len > end)
			break;

		if (attr_type == STUN_ATTR_MESSAGE_INTEGRITY) {
			if (attr_len != 20)
				return -1;
			mi_attr = p + 4;
			mi_offset = (size_t)(p - msg);
			break;
		}
		if (p + 4 + padded > end)
			break;
		p += 4 + padded;
	}

	if (!mi_attr)
		return -1;

	/* Compute HMAC-SHA1 over message up to MI, with length adjusted
	 * to include MI attribute (24 bytes: 4 header + 20 value) */
	uint8_t tmp_hdr[STUN_HEADER_SIZE];
	memcpy(tmp_hdr, msg, STUN_HEADER_SIZE);
	uint16_t adj_len = (uint16_t)(mi_offset - STUN_HEADER_SIZE + 24);
	wr16(tmp_hdr + 2, adj_len);

	/* Build message to HMAC: adjusted header + attributes up to MI header */
	size_t hmac_data_len = mi_offset;
	uint8_t hmac_buf[2048];
	if (hmac_data_len > sizeof(hmac_buf))
		return -1;

	memcpy(hmac_buf, tmp_hdr, STUN_HEADER_SIZE);
	memcpy(hmac_buf + STUN_HEADER_SIZE, msg + STUN_HEADER_SIZE,
	       hmac_data_len - STUN_HEADER_SIZE);

	uint8_t expected[20];
	rwd_hmac_sha1((const uint8_t *)ice_pwd, strlen(ice_pwd), hmac_buf, hmac_data_len, expected);

	/* Constant-time compare */
	uint8_t diff = 0;
	for (int i = 0; i < 20; i++)
		diff |= mi_attr[i] ^ expected[i];
	return diff == 0 ? 0 : -1;
}

/* ── Extract USERNAME from STUN message ── */

static int stun_get_username(const uint8_t *msg, size_t msg_len, char *buf, size_t buf_size)
{
	const uint8_t *p = msg + STUN_HEADER_SIZE;
	const uint8_t *end = msg + msg_len;

	while (p + 4 <= end) {
		uint16_t attr_type = rd16(p);
		uint16_t attr_len = rd16(p + 2);
		size_t padded = (attr_len + 3) & ~3u;

		if (p + 4 + attr_len > end)
			break;

		if (attr_type == STUN_ATTR_USERNAME) {
			if (attr_len >= buf_size)
				return -1;
			memcpy(buf, p + 4, attr_len);
			buf[attr_len] = '\0';
			return 0;
		}
		if (p + 4 + padded > end)
			break;
		p += 4 + padded;
	}
	return -1;
}

/* ── Build and send STUN Binding Response ── */

static int stun_send_response(rwd_server_t *srv, const uint8_t *request,
			      const struct sockaddr_storage *to, socklen_t to_len,
			      const char *ice_pwd)
{
	uint8_t resp[256];
	size_t off = 0;

	/* Header: type=0x0101, length TBD, magic cookie, transaction ID */
	wr16(resp + off, STUN_BINDING_RESPONSE);
	off += 2;
	wr16(resp + off, 0); /* length placeholder */
	off += 2;
	memcpy(resp + off, request + 4, 16); /* magic cookie + transaction ID */
	off += 16;

	/* XOR-MAPPED-ADDRESS */
	wr16(resp + off, STUN_ATTR_XOR_MAPPED_ADDR);
	off += 2;

	if (to->ss_family == AF_INET) {
		const struct sockaddr_in *a4 = (const struct sockaddr_in *)to;
		wr16(resp + off, 8); /* attribute length */
		off += 2;
		resp[off++] = 0;    /* reserved */
		resp[off++] = 0x01; /* family: IPv4 */
		uint16_t xport = ntohs(a4->sin_port) ^ (uint16_t)(STUN_MAGIC_COOKIE >> 16);
		wr16(resp + off, xport);
		off += 2;
		uint32_t xaddr = ntohl(a4->sin_addr.s_addr) ^ STUN_MAGIC_COOKIE;
		wr32(resp + off, xaddr);
		off += 4;
	} else {
		const struct sockaddr_in6 *a6 = (const struct sockaddr_in6 *)to;
		wr16(resp + off, 20); /* attribute length */
		off += 2;
		resp[off++] = 0;
		resp[off++] = 0x02; /* family: IPv6 */
		uint16_t xport = ntohs(a6->sin6_port) ^ (uint16_t)(STUN_MAGIC_COOKIE >> 16);
		wr16(resp + off, xport);
		off += 2;
		/* XOR with magic cookie + transaction ID */
		uint8_t xor_key[16];
		memcpy(xor_key, request + 4, 16); /* magic cookie + txn ID */
		for (int i = 0; i < 16; i++)
			resp[off + i] = a6->sin6_addr.s6_addr[i] ^ xor_key[i];
		off += 16;
	}

	/* MESSAGE-INTEGRITY: HMAC-SHA1 over message with adjusted length */
	size_t mi_offset = off;
	uint16_t adj_len = (uint16_t)(mi_offset - STUN_HEADER_SIZE + 24);
	wr16(resp + 2, adj_len); /* adjust length in header */

	uint8_t hmac[20];
	rwd_hmac_sha1((const uint8_t *)ice_pwd, strlen(ice_pwd), resp, mi_offset, hmac);

	wr16(resp + off, STUN_ATTR_MESSAGE_INTEGRITY);
	off += 2;
	wr16(resp + off, 20);
	off += 2;
	memcpy(resp + off, hmac, 20);
	off += 20;

	/* FINGERPRINT: CRC32 XOR 0x5354554E */
	uint16_t fp_adj_len = (uint16_t)(off - STUN_HEADER_SIZE + 8);
	wr16(resp + 2, fp_adj_len);

	uint32_t crc = crc32_compute(resp, off) ^ STUN_FINGERPRINT_XOR;
	wr16(resp + off, STUN_ATTR_FINGERPRINT);
	off += 2;
	wr16(resp + off, 4);
	off += 2;
	wr32(resp + off, crc);
	off += 4;

	/* Final length (everything after header, before we added FP was right) */
	wr16(resp + 2, (uint16_t)(off - STUN_HEADER_SIZE));

	ssize_t sent = sendto(srv->udp_fd, resp, off, 0, (const struct sockaddr *)to, to_len);
	return sent > 0 ? 0 : -1;
}

/* ── Send ICE connectivity check for NAT hole punching ── */

int rwd_ice_send_check(rwd_server_t *srv, rwd_client_t *c, const char *dest_ip, uint16_t dest_port)
{
	struct sockaddr_in dest;
	memset(&dest, 0, sizeof(dest));
	dest.sin_family = AF_INET;
	dest.sin_port = htons(dest_port);
	if (inet_pton(AF_INET, dest_ip, &dest.sin_addr) != 1)
		return -1;

	uint8_t msg[256];
	size_t off = 0;

	/* STUN header */
	wr16(msg + off, STUN_BINDING_REQUEST);
	off += 2;
	wr16(msg + off, 0); /* length placeholder */
	off += 2;
	wr32(msg + off, STUN_MAGIC_COOKIE);
	off += 4;
	rwd_random_bytes(msg + off, 12);
	off += 12;

	/* USERNAME: "remote_ufrag:local_ufrag" (from receiver's perspective) */
	char username[128];
	int ulen_i = snprintf(username, sizeof(username), "%s:%s", c->remote_ufrag, c->local_ufrag);
	if (ulen_i < 0 || (size_t)ulen_i >= sizeof(username))
		return -1;
	size_t ulen = (size_t)ulen_i;
	size_t ulen_padded = (ulen + 3) & ~3u;
	/* 20 (hdr) + 4+ulen_padded (USERNAME) + 12 (ICE-CONTROLLED) + 8 (PRIORITY)
	 * + 24 (MI) + 8 (FP) must fit in msg[256] */
	if (20 + 4 + ulen_padded + 12 + 8 + 24 + 8 > sizeof(msg))
		return -1;
	wr16(msg + off, STUN_ATTR_USERNAME);
	off += 2;
	wr16(msg + off, (uint16_t)ulen);
	off += 2;
	memcpy(msg + off, username, ulen);
	off += ulen;
	while (off & 3)
		msg[off++] = 0;

	/* ICE-CONTROLLED: 0x8029, 8-byte tiebreaker */
	wr16(msg + off, 0x8029);
	off += 2;
	wr16(msg + off, 8);
	off += 2;
	rwd_random_bytes(msg + off, 8);
	off += 8;

	/* PRIORITY: 0x0024, srflx priority */
	wr16(msg + off, 0x0024);
	off += 2;
	wr16(msg + off, 4);
	off += 2;
	wr32(msg + off, 1694498815);
	off += 4;

	/* MESSAGE-INTEGRITY */
	uint16_t mi_adj_len = (uint16_t)(off - STUN_HEADER_SIZE + 24);
	wr16(msg + 2, mi_adj_len);

	uint8_t hmac[20];
	rwd_hmac_sha1((const uint8_t *)c->remote_pwd, strlen(c->remote_pwd), msg, off, hmac);

	wr16(msg + off, STUN_ATTR_MESSAGE_INTEGRITY);
	off += 2;
	wr16(msg + off, 20);
	off += 2;
	memcpy(msg + off, hmac, 20);
	off += 20;

	/* FINGERPRINT */
	uint16_t fp_adj_len = (uint16_t)(off - STUN_HEADER_SIZE + 8);
	wr16(msg + 2, fp_adj_len);

	uint32_t crc = crc32_compute(msg, off) ^ STUN_FINGERPRINT_XOR;
	wr16(msg + off, STUN_ATTR_FINGERPRINT);
	off += 2;
	wr16(msg + off, 4);
	off += 2;
	wr32(msg + off, crc);
	off += 4;

	/* Final length */
	wr16(msg + 2, (uint16_t)(off - STUN_HEADER_SIZE));

	ssize_t sent = sendto(srv->udp_fd, msg, off, 0, (struct sockaddr *)&dest, sizeof(dest));
	RSS_DEBUG("ICE: sent check to %s:%u (%zd bytes)", dest_ip, dest_port, sent);
	return sent > 0 ? 0 : -1;
}

/* ── Main ICE packet handler ── */

int rwd_ice_process(rwd_server_t *srv, const uint8_t *buf, size_t len,
		    const struct sockaddr_storage *from, socklen_t from_len)
{
	if (len < STUN_HEADER_SIZE)
		return -1;

	uint16_t msg_type = rd16(buf);
	uint16_t msg_len = rd16(buf + 2);
	uint32_t cookie = rd32(buf + 4);

	if (cookie != STUN_MAGIC_COOKIE)
		return -1;
	if (msg_type != STUN_BINDING_REQUEST)
		return -1;
	if ((size_t)(STUN_HEADER_SIZE + msg_len) > len)
		return -1;

	/* Extract USERNAME: format is "local_ufrag:remote_ufrag" */
	char username[128];
	if (stun_get_username(buf, STUN_HEADER_SIZE + msg_len, username, sizeof(username)) != 0) {
		RSS_DEBUG("STUN: no USERNAME attribute");
		return -1;
	}

	/* Find the colon separator */
	char *colon = strchr(username, ':');
	if (!colon) {
		RSS_DEBUG("STUN: invalid USERNAME format: %s", username);
		return -1;
	}
	*colon = '\0';
	const char *local_ufrag = username;

	/* Find matching client by local ICE ufrag */
	rwd_client_t *client = NULL;
	pthread_mutex_lock(&srv->clients_lock);
	for (int i = 0; i < RWD_MAX_CLIENTS; i++) {
		if (srv->clients[i] && strcmp(srv->clients[i]->local_ufrag, local_ufrag) == 0) {
			client = srv->clients[i];
			break;
		}
	}
	pthread_mutex_unlock(&srv->clients_lock);

	if (!client) {
		RSS_DEBUG("STUN: no client with ufrag %s", local_ufrag);
		return -1;
	}

	/* Verify MESSAGE-INTEGRITY with our local ICE password */
	if (stun_verify_integrity(buf, STUN_HEADER_SIZE + msg_len, client->local_pwd) != 0) {
		RSS_WARN("STUN: MESSAGE-INTEGRITY check failed for %s", local_ufrag);
		return -1;
	}

	/* Record client's transport address (first verified STUN binds the address) */
	if (!client->ice_verified) {
		memcpy(&client->addr, from, from_len);
		client->addr_len = from_len;
		client->ice_verified = true;
		RSS_INFO("ICE: verified client %s", client->session_id);
	}

	/* Track consent freshness (RFC 7675) */
	client->last_stun_at = rss_timestamp_us();

	/* Send binding response */
	return stun_send_response(srv, buf, from, from_len, client->local_pwd);
}

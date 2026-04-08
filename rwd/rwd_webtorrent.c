/*
 * rwd_webtorrent.c -- WebTorrent tracker signaling for external WebRTC
 *
 * Connects to a public WebTorrent tracker via TLS WebSocket, announces
 * a share room, and handles incoming SDP offers from remote viewers.
 * This enables WebRTC streaming without port forwarding.
 *
 * Uses STUN to discover the camera's server-reflexive (public) address
 * and sends ICE connectivity checks to punch through NAT.
 *
 * Build: enabled by RAPTOR_WEBTORRENT / -DRAPTOR_WEBTORRENT.
 */

#ifdef RAPTOR_WEBTORRENT

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/md.h>
#include <mbedtls/x509_crt.h>

#include "rwd.h"

/* ── TLS WebSocket state ── */

typedef struct {
	int fd;
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context ctr_drbg;
	mbedtls_x509_crt ca_cert;
	bool has_ca;
	bool connected;
} wt_tls_t;

/* ── BIO callbacks for TCP socket ── */

static int wt_bio_send(void *ctx, const unsigned char *buf, size_t len)
{
	int fd = *(int *)ctx;
	ssize_t ret = write(fd, buf, len);
	if (ret < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return MBEDTLS_ERR_SSL_WANT_WRITE;
		return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
	}
	return (int)ret;
}

static int wt_bio_recv(void *ctx, unsigned char *buf, size_t len)
{
	int fd = *(int *)ctx;
	ssize_t ret = read(fd, buf, len);
	if (ret < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return MBEDTLS_ERR_SSL_WANT_READ;
		return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
	}
	if (ret == 0)
		return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
	return (int)ret;
}

static int wt_bio_recv_timeout(void *ctx, unsigned char *buf, size_t len, uint32_t timeout)
{
	int fd = *(int *)ctx;
	struct pollfd pfd = {.fd = fd, .events = POLLIN};
	int ret = poll(&pfd, 1, (int)timeout);
	if (ret == 0)
		return MBEDTLS_ERR_SSL_TIMEOUT;
	if (ret < 0)
		return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
	return wt_bio_recv(ctx, buf, len);
}

/* ── TCP connect ── */

static int tcp_connect(const char *host, const char *port)
{
	struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
	struct addrinfo *res, *rp;

	if (getaddrinfo(host, port, &hints, &res) != 0)
		return -1;

	int fd = -1;
	for (rp = res; rp; rp = rp->ai_next) {
		fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (fd < 0)
			continue;

		struct timeval tv = {.tv_sec = 10};
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

		if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0)
			break;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	return fd;
}

/* ── TLS connect / disconnect ── */

static int wt_tls_connect(wt_tls_t *tls, const char *host, const char *port, bool tls_verify)
{
	int ret;
	memset(tls, 0, sizeof(*tls));
	tls->fd = -1;

	mbedtls_ssl_init(&tls->ssl);
	mbedtls_ssl_config_init(&tls->conf);
	mbedtls_entropy_init(&tls->entropy);
	mbedtls_ctr_drbg_init(&tls->ctr_drbg);
	mbedtls_x509_crt_init(&tls->ca_cert);
	tls->has_ca = false;

	ret = mbedtls_ctr_drbg_seed(&tls->ctr_drbg, mbedtls_entropy_func, &tls->entropy,
				    (const uint8_t *)"wt", 2);
	if (ret != 0)
		goto fail;

	tls->fd = tcp_connect(host, port);
	if (tls->fd < 0) {
		RSS_ERROR("webtorrent: TCP connect to %s:%s failed", host, port);
		goto fail;
	}

	ret = mbedtls_ssl_config_defaults(&tls->conf, MBEDTLS_SSL_IS_CLIENT,
					  MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
	if (ret != 0)
		goto fail;

	/* Try to load system CA bundle for tracker cert verification.
	 * If unavailable or tls_verify is disabled, fall back to no verification
	 * (DTLS-SRTP still secures the media path regardless). */
	if (tls_verify &&
	    mbedtls_x509_crt_parse_file(&tls->ca_cert, "/etc/ssl/certs/ca-certificates.crt") >= 0) {
		mbedtls_ssl_conf_ca_chain(&tls->conf, &tls->ca_cert, NULL);
		mbedtls_ssl_conf_authmode(&tls->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
		tls->has_ca = true;
	} else {
		if (tls_verify)
			RSS_WARN("webtorrent: CA bundle not found, TLS verify disabled");
		mbedtls_ssl_conf_authmode(&tls->conf, MBEDTLS_SSL_VERIFY_NONE);
	}
	mbedtls_ssl_conf_rng(&tls->conf, mbedtls_ctr_drbg_random, &tls->ctr_drbg);
	mbedtls_ssl_conf_read_timeout(&tls->conf, 5000);

	ret = mbedtls_ssl_setup(&tls->ssl, &tls->conf);
	if (ret != 0)
		goto fail;

	mbedtls_ssl_set_hostname(&tls->ssl, host);
	mbedtls_ssl_set_bio(&tls->ssl, &tls->fd, wt_bio_send, wt_bio_recv, wt_bio_recv_timeout);

	while ((ret = mbedtls_ssl_handshake(&tls->ssl)) != 0) {
		if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
			RSS_ERROR("webtorrent: TLS handshake failed: -0x%04x", -ret);
			goto fail;
		}
	}

	tls->connected = true;
	return 0;

fail:
	if (tls->fd >= 0)
		close(tls->fd);
	tls->fd = -1;
	mbedtls_ssl_free(&tls->ssl);
	mbedtls_ssl_config_free(&tls->conf);
	mbedtls_x509_crt_free(&tls->ca_cert);
	mbedtls_ctr_drbg_free(&tls->ctr_drbg);
	mbedtls_entropy_free(&tls->entropy);
	return -1;
}

static void wt_tls_close(wt_tls_t *tls)
{
	if (tls->connected)
		mbedtls_ssl_close_notify(&tls->ssl);
	tls->connected = false;
	mbedtls_ssl_free(&tls->ssl);
	mbedtls_ssl_config_free(&tls->conf);
	mbedtls_x509_crt_free(&tls->ca_cert);
	mbedtls_ctr_drbg_free(&tls->ctr_drbg);
	mbedtls_entropy_free(&tls->entropy);
	if (tls->fd >= 0)
		close(tls->fd);
	tls->fd = -1;
}

static int wt_tls_write_all(wt_tls_t *tls, const uint8_t *buf, size_t len)
{
	size_t written = 0;
	int retries = 0;
	while (written < len) {
		int ret = mbedtls_ssl_write(&tls->ssl, buf + written, len - written);
		if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
			if (++retries > 1000)
				return -1;
			continue;
		}
		if (ret < 0)
			return -1;
		written += ret;
		retries = 0;
	}
	return 0;
}

/* Read exactly n bytes. Returns 0 on success, 1 on timeout, -1 on error. */
static int tls_read_exact(wt_tls_t *tls, uint8_t *buf, size_t need, bool first_byte)
{
	size_t got = 0;
	int retries = 0;
	while (got < need) {
		int ret = mbedtls_ssl_read(&tls->ssl, buf + got, need - got);
		if (ret == MBEDTLS_ERR_SSL_WANT_READ) {
			if (++retries > 1000)
				return -1;
			continue;
		}
		if (ret == MBEDTLS_ERR_SSL_TIMEOUT)
			return (first_byte && got == 0) ? 1 : -1;
		if (ret <= 0)
			return -1;
		got += ret;
		first_byte = false;
		retries = 0;
	}
	return 0;
}

/* ── WebSocket framing ── */

static int ws_send_text(wt_tls_t *tls, const char *msg, size_t len)
{
	uint8_t frame[14];
	size_t hdr_len = 2;

	frame[0] = 0x81; /* FIN + TEXT */

	if (len < 126) {
		frame[1] = 0x80 | (uint8_t)len;
	} else if (len <= 65535) {
		frame[1] = 0x80 | 126;
		frame[2] = (uint8_t)(len >> 8);
		frame[3] = (uint8_t)len;
		hdr_len = 4;
	} else {
		return -1; /* messages > 64KB not supported */
	}

	uint8_t mask[4] = {0};
	rwd_random_bytes(mask, 4);
	memcpy(frame + hdr_len, mask, 4);
	hdr_len += 4;

	if (wt_tls_write_all(tls, frame, hdr_len) != 0)
		return -1;

	/* Send masked payload in chunks */
	uint8_t chunk[1024];
	size_t sent = 0;
	while (sent < len) {
		size_t n = len - sent;
		if (n > sizeof(chunk))
			n = sizeof(chunk);
		for (size_t i = 0; i < n; i++)
			chunk[i] = ((const uint8_t *)msg)[sent + i] ^ mask[(sent + i) & 3];
		if (wt_tls_write_all(tls, chunk, n) != 0)
			return -1;
		sent += n;
	}
	return 0;
}

/* Receive one WebSocket frame. Returns payload length, 0 on timeout, -1 on error/close.
 * Handles ping internally by sending pong. */
static int ws_recv_frame(wt_tls_t *tls, char *buf, size_t buf_size)
{
	int ctrl_count = 0; /* guard against control frame floods */
	for (;;) {
		uint8_t hdr[2];
		int ret = tls_read_exact(tls, hdr, 2, true);
		if (ret == 1)
			return 0; /* timeout, not an error */
		if (ret < 0)
			return -1;

		uint8_t opcode = hdr[0] & 0x0F;
		bool masked = (hdr[1] & 0x80) != 0;
		uint64_t payload_len = hdr[1] & 0x7F;

		if (payload_len == 126) {
			uint8_t ext[2];
			if (tls_read_exact(tls, ext, 2, false) != 0)
				return -1;
			payload_len = ((uint16_t)ext[0] << 8) | ext[1];
		} else if (payload_len == 127) {
			uint8_t ext[8];
			if (tls_read_exact(tls, ext, 8, false) != 0)
				return -1;
			/* RFC 6455 §5.2: MSB must be 0 */
			if (ext[0] & 0x80)
				return -1;
			payload_len = 0;
			for (int i = 0; i < 8; i++)
				payload_len = (payload_len << 8) | ext[i];
		}

		uint8_t mask_key[4] = {0};
		if (masked) {
			if (tls_read_exact(tls, mask_key, 4, false) != 0)
				return -1;
		}

		/* Reject absurdly large frames — tracker messages are
		 * at most a few KB; anything over 64KB is malicious. */
		if (payload_len > 65536)
			return -1;

		/* Skip oversized frames (within the 64KB cap) */
		if (payload_len >= buf_size) {
			uint8_t skip[256];
			uint64_t rem = payload_len;
			while (rem > 0) {
				size_t n = rem > sizeof(skip) ? sizeof(skip) : (size_t)rem;
				if (tls_read_exact(tls, skip, n, false) != 0)
					return -1;
				rem -= n;
			}
			continue;
		}

		if (payload_len > 0) {
			if (tls_read_exact(tls, (uint8_t *)buf, (size_t)payload_len, false) != 0)
				return -1;
			if (masked)
				for (size_t i = 0; i < payload_len; i++)
					buf[i] ^= mask_key[i & 3];
		}
		buf[payload_len] = '\0';

		if (opcode == 0x09) { /* Ping → Pong (RFC 6455 5.5.3: echo payload) */
			if (++ctrl_count > 100)
				return -1; /* ping flood */
			/* RFC 6455: ping payload max 125 bytes (control frame limit) */
			uint8_t pong[131]; /* 6 header + 125 payload */
			uint8_t pong_mask[4] = {0};
			rwd_random_bytes(pong_mask, 4);
			size_t plen = payload_len < 125 ? (size_t)payload_len : 0;
			pong[0] = 0x8A; /* FIN + PONG */
			pong[1] = 0x80 | (uint8_t)plen;
			memcpy(pong + 2, pong_mask, 4);
			for (size_t i = 0; i < plen; i++)
				pong[6 + i] = (uint8_t)buf[i] ^ pong_mask[i & 3];
			wt_tls_write_all(tls, pong, 6 + plen);
			continue;
		}
		if (opcode == 0x08) /* Close */
			return -1;
		if (opcode == 0x01) /* Text */
			return (int)payload_len;
		/* Skip unknown opcodes */
		if (++ctrl_count > 100)
			return -1;
	}
}

/* ── WebSocket HTTP upgrade ── */

static int ws_upgrade(wt_tls_t *tls, const char *host, const char *path)
{
	/* Reject paths with CRLF to prevent HTTP header injection */
	if (strchr(path, '\r') || strchr(path, '\n'))
		return -1;

	/* Generate Sec-WebSocket-Key */
	uint8_t key_raw[16];
	rwd_random_bytes(key_raw, sizeof(key_raw));

	static const char b64[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	char key_b64[25];
	int j = 0;
	for (int i = 0; i < 16; i += 3) {
		uint32_t v = (uint32_t)key_raw[i] << 16;
		if (i + 1 < 16)
			v |= (uint32_t)key_raw[i + 1] << 8;
		if (i + 2 < 16)
			v |= key_raw[i + 2];
		key_b64[j++] = b64[(v >> 18) & 0x3F];
		key_b64[j++] = b64[(v >> 12) & 0x3F];
		key_b64[j++] = (i + 1 < 16) ? b64[(v >> 6) & 0x3F] : '=';
		key_b64[j++] = (i + 2 < 16) ? b64[v & 0x3F] : '=';
	}
	key_b64[j] = '\0';

	char req[512];
	int req_len = snprintf(req, sizeof(req),
			       "GET %s HTTP/1.1\r\n"
			       "Host: %s\r\n"
			       "Upgrade: websocket\r\n"
			       "Connection: Upgrade\r\n"
			       "Sec-WebSocket-Key: %s\r\n"
			       "Sec-WebSocket-Version: 13\r\n"
			       "\r\n",
			       path, host, key_b64);

	if (wt_tls_write_all(tls, (const uint8_t *)req, req_len) != 0)
		return -1;

	/* Read response until \r\n\r\n with total timeout guard.
	 * Individual reads have a 5s timeout (from conf), but a slow-drip
	 * attack could send one byte per interval. Cap total elapsed time. */
	char resp[1024];
	size_t resp_len = 0;
	int64_t deadline = rss_timestamp_us() + 10 * 1000000LL; /* 10s total */
	while (resp_len < sizeof(resp) - 1) {
		if (rss_timestamp_us() > deadline)
			return -1;
		int ret = mbedtls_ssl_read(&tls->ssl, (uint8_t *)resp + resp_len, 1);
		if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_TIMEOUT)
			continue;
		if (ret <= 0)
			return -1;
		resp_len += ret;
		resp[resp_len] = '\0';
		if (strstr(resp, "\r\n\r\n"))
			break;
	}

	/* Check for "101" in the status line (first line only) */
	return strncmp(resp, "HTTP/1.1 101", 12) == 0 ? 0 : -1;
}

/* ── JSON helpers (minimal, for tracker protocol) ── */

static size_t json_escape(const char *in, char *out, size_t out_size)
{
	if (out_size == 0)
		return 0;
	size_t w = 0;
	for (size_t i = 0; in[i]; i++) {
		char c = in[i];
		bool esc = (c == '"' || c == '\\' || c == '\r' || c == '\n');
		size_t need = esc ? 2 : 1;
		if (w + need >= out_size)
			break;
		if (c == '"' || c == '\\') {
			out[w++] = '\\';
			out[w++] = c;
		} else if (c == '\r') {
			out[w++] = '\\';
			out[w++] = 'r';
		} else if (c == '\n') {
			out[w++] = '\\';
			out[w++] = 'n';
		} else {
			out[w++] = c;
		}
	}
	out[w] = '\0';
	return w;
}

/* Find key at JSON key position (not inside a string value).
 * Counts unescaped quotes before each match — even = key position. */
static const char *json_find_key(const char *json, const char *needle, size_t needle_len)
{
	const char *p = json;
	while ((p = strstr(p, needle)) != NULL) {
		/* Count unescaped quotes from start to match position */
		int quotes = 0;
		for (const char *q = json; q < p; q++) {
			if (*q == '"' && (q == json || q[-1] != '\\'))
				quotes++;
		}
		if (quotes % 2 == 0)
			return p; /* outside a string — real key */
		p += needle_len;
	}
	return NULL;
}

static int json_get_str(const char *json, const char *key, char *out, size_t out_size)
{
	if (out_size == 0)
		return -1;
	char needle[128];
	int nlen = snprintf(needle, sizeof(needle), "\"%s\":", key);
	const char *p = json_find_key(json, needle, (size_t)nlen);
	if (!p)
		return -1;
	p += nlen;
	while (*p == ' ')
		p++;
	if (*p != '"')
		return -1;
	p++;
	size_t i = 0;
	while (*p && *p != '"' && i < out_size - 1) {
		if (*p == '\\' && p[1]) {
			p++;
			switch (*p) {
			case 'r':
				out[i++] = '\r';
				break;
			case 'n':
				out[i++] = '\n';
				break;
			case 't':
				out[i++] = '\t';
				break;
			default:
				out[i++] = *p;
				break;
			}
		} else {
			out[i++] = *p;
		}
		p++;
	}
	out[i] = '\0';
	return 0;
}

/* Extract nested JSON object as raw string (brace-matched). */
static int json_get_object(const char *json, const char *key, char *out, size_t out_size)
{
	char needle[128];
	int nlen = snprintf(needle, sizeof(needle), "\"%s\":", key);
	const char *p = json_find_key(json, needle, (size_t)nlen);
	if (!p)
		return -1;
	p += nlen;
	while (*p == ' ')
		p++;
	if (*p != '{')
		return -1;

	int depth = 0;
	const char *start = p;
	while (*p) {
		if (*p == '{')
			depth++;
		else if (*p == '}') {
			depth--;
			if (depth == 0) {
				size_t len = (size_t)(p - start + 1);
				if (len >= out_size)
					return -1;
				memcpy(out, start, len);
				out[len] = '\0';
				return 0;
			}
		} else if (*p == '"') {
			p++;
			while (*p && *p != '"') {
				if (*p == '\\' && p[1])
					p++;
				p++;
			}
		}
		p++;
	}
	return -1;
}

/* ── STUN client (discover server-reflexive address) ── */

int rwd_stun_discover_srflx(int udp_fd, const char *server, int port, char *ip_out, size_t ip_size,
			    uint16_t *port_out)
{
	struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_DGRAM};
	struct addrinfo *res;
	char port_str[16];
	snprintf(port_str, sizeof(port_str), "%d", port);

	if (getaddrinfo(server, port_str, &hints, &res) != 0) {
		RSS_ERROR("webtorrent: failed to resolve STUN server %s", server);
		return -1;
	}

	/* Minimal STUN Binding Request (no attributes) */
	uint8_t req[20];
	req[0] = 0x00;
	req[1] = 0x01; /* Binding Request */
	req[2] = 0x00;
	req[3] = 0x00; /* Length = 0 */
	req[4] = 0x21;
	req[5] = 0x12;
	req[6] = 0xA4;
	req[7] = 0x42; /* Magic Cookie */
	rwd_random_bytes(req + 8, 12);

	/* Try each resolved address (IPv6 first if available) */
	for (struct addrinfo *rp = res; rp; rp = rp->ai_next) {
		for (int attempt = 0; attempt < 3; attempt++) {
			sendto(udp_fd, req, sizeof(req), 0, rp->ai_addr, rp->ai_addrlen);

			struct pollfd pfd = {.fd = udp_fd, .events = POLLIN};
			if (poll(&pfd, 1, 1500) <= 0)
				continue;

			uint8_t buf[256];
			struct sockaddr_storage from;
			socklen_t from_len = sizeof(from);
			ssize_t n = recvfrom(udp_fd, buf, sizeof(buf), 0, (struct sockaddr *)&from,
					     &from_len);
			if (n < 20)
				continue;
			if (buf[0] != 0x01 || buf[1] != 0x01)
				continue;
			if (memcmp(buf + 8, req + 8, 12) != 0)
				continue;

			uint16_t msg_len = ((uint16_t)buf[2] << 8) | buf[3];
			const uint8_t *p = buf + 20;
			const uint8_t *end = buf + 20 + msg_len;
			if (end > buf + n)
				end = buf + n;

			while (p + 4 <= end) {
				uint16_t attr_type = ((uint16_t)p[0] << 8) | p[1];
				uint16_t attr_len = ((uint16_t)p[2] << 8) | p[3];
				if (p + 4 + attr_len > end)
					break;

				/* XOR-MAPPED-ADDRESS (RFC 5389 §15.2) */
				if (attr_type == 0x0020 && attr_len >= 8) {
					uint8_t family = p[5];
					uint16_t xport = (((uint16_t)p[6] << 8) | p[7]) ^ 0x2112;

					if (family == 0x01 && attr_len >= 8) {
						/* IPv4: 4-byte address XOR'd with magic cookie */
						uint32_t xaddr = (((uint32_t)p[8] << 24) |
								  ((uint32_t)p[9] << 16) |
								  ((uint32_t)p[10] << 8) | p[11]) ^
								 0x2112A442;
						struct in_addr addr;
						addr.s_addr = htonl(xaddr);
						inet_ntop(AF_INET, &addr, ip_out, ip_size);
						*port_out = xport;
						freeaddrinfo(res);
						return 0;
					}

					if (family == 0x02 && attr_len >= 20) {
						/* IPv6: 16-byte address XOR'd with
						 * magic cookie (4) + txn ID (12) */
						uint8_t xor_key[16];
						memcpy(xor_key, req + 4, 4);	  /* magic */
						memcpy(xor_key + 4, req + 8, 12); /* txn ID */
						struct in6_addr addr6;
						for (int i = 0; i < 16; i++)
							addr6.s6_addr[i] = p[8 + i] ^ xor_key[i];
						inet_ntop(AF_INET6, &addr6, ip_out, ip_size);
						*port_out = xport;
						freeaddrinfo(res);
						return 0;
					}
				}
				p += 4 + ((attr_len + 3) & ~3u);
			}
		}
	}

	freeaddrinfo(res);
	return -1;
}

/* ── info_hash computation ── */

/* Base64 encode (no padding variant not needed — standard base64 with padding) */
static size_t b64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_size)
{
	static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t j = 0;
	for (size_t i = 0; i < in_len && j + 4 < out_size; i += 3) {
		uint32_t v = (uint32_t)in[i] << 16;
		if (i + 1 < in_len)
			v |= (uint32_t)in[i + 1] << 8;
		if (i + 2 < in_len)
			v |= in[i + 2];
		out[j++] = t[(v >> 18) & 0x3F];
		out[j++] = t[(v >> 12) & 0x3F];
		out[j++] = (i + 1 < in_len) ? t[(v >> 6) & 0x3F] : '=';
		out[j++] = (i + 2 < in_len) ? t[v & 0x3F] : '=';
	}
	out[j] = '\0';
	return j;
}

/* info_hash = base64(SHA-256("raptor:" + key)) */
static void compute_info_hash(const char *share_key, char *b64_out)
{
	char input[128]; /* "raptor:" (7) + share_key (up to 36 with prefix) + null */
	snprintf(input, sizeof(input), "raptor:%s", share_key);

	uint8_t hash[32];
	const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
	mbedtls_md(md, (const uint8_t *)input, strlen(input), hash);

	b64_encode(hash, 32, b64_out, 48);
}

static void generate_random_id(char *out, size_t len)
{
	static const char cs[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	/* 62 chars: reject values >= 248 to eliminate modulo bias (256 % 62 = 8) */
	size_t n = len - 1;
	uint8_t raw[64];	       /* batch read — avoids one syscall per character */
	size_t w = 0, r = sizeof(raw); /* r starts past end to force first fill */
	while (w < n) {
		if (r >= sizeof(raw)) {
			rwd_random_bytes(raw, sizeof(raw));
			r = 0;
		}
		if (raw[r] < 248)
			out[w++] = cs[raw[r] % 62];
		r++;
	}
	out[w] = '\0';
}

/* ── Tracker protocol ── */

static int wt_announce(wt_tls_t *tls, const char *info_hash, const char *peer_id)
{
	char msg[512];
	snprintf(msg, sizeof(msg),
		 "{\"action\":\"announce\",\"info_hash\":\"%s\",\"peer_id\":\"%s\","
		 "\"numwant\":0,\"uploaded\":0,\"downloaded\":0,\"left\":0,"
		 "\"event\":\"started\"}",
		 info_hash, peer_id);
	return ws_send_text(tls, msg, strlen(msg));
}

static int wt_send_answer(wt_tls_t *tls, const char *info_hash, const char *peer_id,
			  const char *to_peer_id, const char *offer_id, const char *sdp_answer)
{
	/* Escape all strings that may contain special chars.
	 * to_peer_id and offer_id come from the tracker (untrusted). */
	char esc_to_peer[128], esc_offer_id[128];
	json_escape(to_peer_id, esc_to_peer, sizeof(esc_to_peer));
	json_escape(offer_id, esc_offer_id, sizeof(esc_offer_id));

	/* Heap-allocate escaped SDP + message buffer together */
	size_t sdp_len = strlen(sdp_answer);
	size_t esc_sdp_size = sdp_len * 2 + 1;
	char *esc_sdp = malloc(esc_sdp_size);
	if (!esc_sdp)
		return -1;
	json_escape(sdp_answer, esc_sdp, esc_sdp_size);

	size_t msg_size = strlen(esc_sdp) + strlen(esc_to_peer) + strlen(esc_offer_id) + 512;
	char *msg = malloc(msg_size);
	if (!msg) {
		free(esc_sdp);
		return -1;
	}

	snprintf(msg, msg_size,
		 "{\"action\":\"announce\",\"info_hash\":\"%s\",\"peer_id\":\"%s\","
		 "\"to_peer_id\":\"%s\",\"answer\":{\"type\":\"answer\",\"sdp\":\"%s\"},"
		 "\"offer_id\":\"%s\"}",
		 info_hash, peer_id, esc_to_peer, esc_sdp, esc_offer_id);
	int ret = ws_send_text(tls, msg, strlen(msg));
	free(msg);
	free(esc_sdp);
	return ret;
}

/* ── Handle incoming offer from tracker ── */

static void handle_tracker_offer(rwd_webtorrent_t *wt, wt_tls_t *tls, const char *json)
{
	rwd_server_t *srv = wt->srv;

	char viewer_peer_id[64] = {0};
	char offer_id[64] = {0};

	json_get_str(json, "peer_id", viewer_peer_id, sizeof(viewer_peer_id));
	json_get_str(json, "offer_id", offer_id, sizeof(offer_id));

	/* Heap-allocate SDP buffers — three 4KB buffers on a thread stack is risky */
	char *offer_obj = malloc(RWD_SDP_BUF_SIZE);
	char *sdp = malloc(RWD_SDP_BUF_SIZE);
	char *sdp_answer = malloc(RWD_SDP_BUF_SIZE);
	if (!offer_obj || !sdp || !sdp_answer) {
		free(offer_obj);
		free(sdp);
		free(sdp_answer);
		RSS_ERROR("webtorrent: OOM in handle_tracker_offer");
		return;
	}

	offer_obj[0] = sdp[0] = sdp_answer[0] = '\0';

	if (json_get_object(json, "offer", offer_obj, RWD_SDP_BUF_SIZE) != 0) {
		RSS_WARN("webtorrent: no offer in tracker message");
		goto out;
	}
	if (json_get_str(offer_obj, "sdp", sdp, RWD_SDP_BUF_SIZE) != 0) {
		RSS_WARN("webtorrent: no sdp in offer");
		goto out;
	}

	/* Parse stream index from offer_id: "sN_..." where N is 0 or 1 */
	int stream_idx = 0;
	if (offer_id[0] == 's' && (offer_id[1] == '0' || offer_id[1] == '1') && offer_id[2] == '_')
		stream_idx = offer_id[1] - '0';

	RSS_DEBUG("webtorrent: offer from peer %.16s... (stream %d)", viewer_peer_id, stream_idx);

	rwd_client_t *c = rwd_client_from_offer(srv, sdp, stream_idx, sdp_answer, RWD_SDP_BUF_SIZE);
	if (!c) {
		RSS_WARN("webtorrent: failed to create client from offer");
		goto out;
	}

	if (wt_send_answer(tls, wt->info_hash, wt->peer_id, viewer_peer_id, offer_id, sdp_answer) !=
	    0) {
		RSS_ERROR("webtorrent: failed to send answer");
		goto out;
	}

	/* NAT hole punch: send ICE checks to browser's candidates */
	for (int i = 0; i < c->offer.candidate_count; i++)
		rwd_ice_send_check(srv, c, c->offer.candidates[i].ip, c->offer.candidates[i].port);

	RSS_DEBUG("webtorrent: answer sent (session %s, %d candidates punched)", c->session_id,
		  c->offer.candidate_count);

out:
	free(offer_obj);
	free(sdp);
	free(sdp_answer);
}

/* ── Parse tracker URL ── */

static int parse_tracker_url(const char *url, char *host, size_t host_size, char *port,
			     size_t port_size, char *path, size_t path_size)
{
	const char *p = url;
	if (strncmp(p, "wss://", 6) == 0)
		p += 6;
	else if (strncmp(p, "ws://", 5) == 0)
		p += 5;
	else
		return -1;

	const char *end = p;
	while (*end && *end != ':' && *end != '/')
		end++;

	size_t hlen = (size_t)(end - p);
	if (hlen >= host_size)
		return -1;
	memcpy(host, p, hlen);
	host[hlen] = '\0';

	if (*end == ':') {
		const char *ps = end + 1;
		const char *pe = ps;
		while (*pe && *pe != '/')
			pe++;
		size_t plen = (size_t)(pe - ps);
		if (plen >= port_size)
			return -1;
		memcpy(port, ps, plen);
		port[plen] = '\0';
		end = pe;
	} else {
		rss_strlcpy(port, "443", port_size);
	}

	if (*end == '/')
		rss_strlcpy(path, end, path_size);
	else
		rss_strlcpy(path, "/", path_size);

	return 0;
}

/* ── Main thread ── */

static void *webtorrent_thread(void *arg)
{
	rwd_webtorrent_t *wt = arg;
	rwd_server_t *srv = wt->srv;
	int backoff = 1;

	char host[128], port[16], path[64];
	if (parse_tracker_url(wt->tracker_url, host, sizeof(host), port, sizeof(port), path,
			      sizeof(path)) != 0) {
		RSS_ERROR("webtorrent: invalid tracker URL: %s", wt->tracker_url);
		return NULL;
	}

	while (wt->running && *srv->running) {
		RSS_DEBUG("webtorrent: connecting to %s:%s%s", host, port, path);

		wt_tls_t tls;
		if (wt_tls_connect(&tls, host, port, wt->tls_verify) != 0) {
			RSS_WARN("webtorrent: connection failed, retry in %ds", backoff);
			for (int i = 0; i < backoff && wt->running && *srv->running; i++)
				sleep(1);
			if (backoff < 30)
				backoff *= 2;
			continue;
		}

		if (ws_upgrade(&tls, host, path) != 0) {
			RSS_WARN("webtorrent: WebSocket upgrade failed");
			wt_tls_close(&tls);
			for (int i = 0; i < backoff && wt->running && *srv->running; i++)
				sleep(1);
			if (backoff < 30)
				backoff *= 2;
			continue;
		}

		RSS_INFO("webtorrent: connected to tracker");
		backoff = 1;

		if (wt_announce(&tls, wt->info_hash, wt->peer_id) != 0) {
			RSS_ERROR("webtorrent: announce failed");
			wt_tls_close(&tls);
			continue;
		}

		RSS_INFO("webtorrent: share URL: %s#key=%s", wt->viewer_base_url, wt->share_key);

		/* Message loop — buf is the largest stack allocation in this thread.
		 * Total thread stack usage: ~10KB (buf + TLS context + locals).
		 * Default pthread stack (128KB on uclibc) is more than sufficient. */
		char buf[8192];
		while (wt->running && *srv->running) {
			int n = ws_recv_frame(&tls, buf, sizeof(buf));
			if (n < 0)
				break; /* error or close */
			if (n == 0)
				continue; /* timeout */

			if (strstr(buf, "\"offer\":"))
				handle_tracker_offer(wt, &tls, buf);
		}

		wt_tls_close(&tls);
		if (wt->running && *srv->running)
			sleep(1);
	}

	RSS_INFO("webtorrent: thread exiting");
	return NULL;
}

/* ── Public API ── */

int rwd_webtorrent_start(rwd_webtorrent_t *wt, rwd_server_t *srv)
{
	wt->srv = srv;
	wt->running = true;

	/* If user sets share_key, prepend a 4-char prefix derived from the key
	 * itself (XOR fold) to prevent tracker collisions between devices that
	 * pick the same key. Deterministic — same key always produces same prefix.
	 * If no key configured, generate a fully random key. */
	const char *cfg_key = rss_config_get_str(srv->cfg, "webtorrent", "share_key", "");
	if (cfg_key[0]) {
		size_t klen = strlen(cfg_key);
		if (klen < 4) {
			RSS_ERROR("webtorrent: share_key must be at least 4 characters");
			return -1;
		}
		if (klen > sizeof(wt->share_key) - 6) { /* 4 prefix + '_' + null */
			RSS_ERROR("webtorrent: share_key too long (max %zu chars)",
				  sizeof(wt->share_key) - 6);
			return -1;
		}
		/* XOR-fold the key into 4 bytes, map to alphanumeric */
		uint8_t fold[4] = {0x5a, 0xa5, 0x3c, 0xc3}; /* non-zero seed */
		for (size_t i = 0; cfg_key[i]; i++)
			fold[i & 3] ^= (uint8_t)cfg_key[i];
		static const char cs[] = "abcdefghijklmnopqrstuvwxyz0123456789";
		char prefix[5];
		for (int i = 0; i < 4; i++)
			prefix[i] = cs[fold[i] % 36];
		prefix[4] = '\0';
		snprintf(wt->share_key, sizeof(wt->share_key), "%s_%s", prefix, cfg_key);
	} else {
		generate_random_id(wt->share_key, sizeof(wt->share_key));
	}
	compute_info_hash(wt->share_key, wt->info_hash);

	uint8_t pid[20];
	rwd_random_bytes(pid, sizeof(pid));
	for (int i = 0; i < 20; i++)
		snprintf(wt->peer_id + i * 2, 3, "%02x", pid[i]);

	/* Discover server-reflexive address via STUN (before threads start) */
	if (rwd_stun_discover_srflx(srv->udp_fd, wt->stun_server, wt->stun_port, srv->srflx_ip,
				    sizeof(srv->srflx_ip), &srv->srflx_port) == 0) {
		srv->has_srflx = true;
		RSS_DEBUG("webtorrent: STUN srflx %s:%u", srv->srflx_ip, srv->srflx_port);
	} else {
		RSS_WARN("webtorrent: STUN discovery failed (external access may not work)");
	}

	if (pthread_create(&wt->thread, NULL, webtorrent_thread, wt) != 0) {
		RSS_ERROR("webtorrent: failed to create thread");
		return -1;
	}

	return 0;
}

void rwd_webtorrent_stop(rwd_webtorrent_t *wt)
{
	wt->running = false;
	pthread_join(wt->thread, NULL);
}

void rwd_webtorrent_rotate_key(rwd_webtorrent_t *wt)
{
	generate_random_id(wt->share_key, sizeof(wt->share_key));
	compute_info_hash(wt->share_key, wt->info_hash);
	RSS_INFO("webtorrent: key rotated, new URL: %s#key=%s", wt->viewer_base_url, wt->share_key);
}

#endif /* RAPTOR_WEBTORRENT */

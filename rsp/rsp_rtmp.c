/*
 * rsp_rtmp.c -- RTMP client for stream push
 *
 * Implements RTMP handshake (C0/C1/C2, S0/S1/S2), AMF0 encoding,
 * chunk stream framing, and FLV-style A/V tag construction for
 * publishing to RTMP/RTMPS servers (YouTube, Twitch, etc).
 *
 * References:
 *   - RTMP specification (Adobe, December 2012)
 *   - FLV file format specification (Adobe)
 *   - ISO 14496-15 (AVCC box structure)
 *   - ISO 14496-3 (AudioSpecificConfig)
 *   - Enhanced RTMP v2 (H.265/HEVC support via FourCC)
 */

#include "rsp_rtmp.h"

#include <rss_common.h>
#include <rss_net.h>

#include <errno.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>

/* ── RTMP constants ── */

#define RTMP_DEFAULT_PORT     1935
#define RTMPS_DEFAULT_PORT    443
#define RTMP_HANDSHAKE_SIZE   1536
#define RTMP_DEFAULT_CHUNK_SZ 128
#define RTMP_CHUNK_SZ_OUT     4096

/* Chunk stream IDs */
#define RTMP_CSID_CONTROL 2
#define RTMP_CSID_COMMAND 3
#define RTMP_CSID_AUDIO	  4
#define RTMP_CSID_VIDEO	  6

/* Message type IDs */
#define RTMP_MSG_SET_CHUNK_SIZE	 1
#define RTMP_MSG_WINDOW_ACK_SIZE 5
#define RTMP_MSG_SET_PEER_BW	 6
#define RTMP_MSG_AUDIO		 8
#define RTMP_MSG_VIDEO		 9
#define RTMP_MSG_AMF0_CMD	 20

/* AMF0 type markers */
#define AMF0_NUMBER  0x00
#define AMF0_STRING  0x02
#define AMF0_OBJECT  0x03
#define AMF0_NULL    0x05
#define AMF0_OBJ_END 0x09

/* FLV audio tag constants */
#define FLV_AUDIO_AAC	     0xA0
#define FLV_AUDIO_44KHZ	     0x0C
#define FLV_AUDIO_STEREO     0x01
#define FLV_AUDIO_16BIT	     0x02
#define FLV_AAC_SEQUENCE_HDR 0
#define FLV_AAC_RAW	     1

/* FLV video tag constants */
#define FLV_VIDEO_KEY	     0x10
#define FLV_VIDEO_INTER	     0x20
#define FLV_VIDEO_H264	     0x07
#define FLV_AVC_SEQUENCE_HDR 0
#define FLV_AVC_NALU	     1

/* Enhanced RTMP (FourCC) for H.265 */
#define FLV_VIDEO_EX_HEADER	     0x80
#define FLV_VIDEO_FOURCC_HEVC	     0x68766331 /* "hvc1" */
#define FLV_PACKET_TYPE_SEQ_START    0
#define FLV_PACKET_TYPE_CODED_FRAMES 1

/* ── I/O helpers ── */

static ssize_t rtmp_send(rsp_rtmp_t *ctx, const void *buf, size_t len)
{
#ifdef RSS_HAS_TLS
	if (ctx->tls_conn)
		return rss_tls_write(ctx->tls_conn, buf, len);
#endif
	const uint8_t *p = buf;
	size_t sent = 0;
	while (sent < len) {
		ssize_t n = write(ctx->fd, p + sent, len - sent);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		sent += (size_t)n;
	}
	return (ssize_t)sent;
}

static ssize_t rtmp_recv(rsp_rtmp_t *ctx, void *buf, size_t len)
{
#ifdef RSS_HAS_TLS
	if (ctx->tls_conn)
		return rss_tls_read(ctx->tls_conn, buf, len);
#endif
	return read(ctx->fd, buf, len);
}

static int rtmp_recv_full(rsp_rtmp_t *ctx, void *buf, size_t len, int timeout_ms)
{
	uint8_t *p = buf;
	size_t got = 0;

	/* Set socket timeout so blocking reads don't hang forever */
	struct timeval tv = {.tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};
	setsockopt(ctx->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	while (got < len) {
		ssize_t n = rtmp_recv(ctx, p + got, len - got);
		if (n <= 0)
			return -1;
		got += (size_t)n;
	}
	return 0;
}

/* ── Big-endian helpers ── */

static inline void put_be16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static inline void put_be24(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 16);
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)v;
}

static inline void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static inline uint32_t get_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* ── AMF0 encoder ── */

static int amf0_write_number(uint8_t *buf, double val)
{
	buf[0] = AMF0_NUMBER;
	union {
		double d;
		uint64_t u;
	} u;
	u.d = val;
	for (int i = 0; i < 8; i++)
		buf[1 + i] = (uint8_t)(u.u >> (56 - i * 8));
	return 9;
}

static int amf0_write_string(uint8_t *buf, const char *s)
{
	uint16_t len = (uint16_t)strlen(s);
	buf[0] = AMF0_STRING;
	put_be16(buf + 1, len);
	memcpy(buf + 3, s, len);
	return 3 + len;
}

static int amf0_write_null(uint8_t *buf)
{
	buf[0] = AMF0_NULL;
	return 1;
}

/* Write object property (key + value already written, this is just the key) */
static int amf0_write_prop_name(uint8_t *buf, const char *name)
{
	uint16_t len = (uint16_t)strlen(name);
	put_be16(buf, len);
	memcpy(buf + 2, name, len);
	return 2 + len;
}

static int amf0_write_obj_end(uint8_t *buf)
{
	buf[0] = 0;
	buf[1] = 0;
	buf[2] = AMF0_OBJ_END;
	return 3;
}

/* ── RTMP chunk stream ── */

/*
 * Send a complete RTMP message, splitting into chunks.
 * Uses fmt=0 (full header) for the first chunk, fmt=3 for continuations.
 */
static int rtmp_send_message(rsp_rtmp_t *ctx, uint8_t csid, uint8_t msg_type, uint32_t stream_id,
			     uint32_t timestamp, const uint8_t *data, uint32_t len)
{
	uint8_t hdr[12];
	uint32_t chunk_sz = ctx->chunk_size;
	uint32_t offset = 0;

	while (offset < len) {
		uint32_t payload = len - offset;
		if (payload > chunk_sz)
			payload = chunk_sz;

		int hdr_len;
		if (offset == 0) {
			/* fmt=0: full header */
			hdr[0] = csid & 0x3F; /* fmt=0 */
			put_be24(hdr + 1, timestamp > 0xFFFFFF ? 0xFFFFFF : timestamp);
			put_be24(hdr + 4, len);
			hdr[7] = msg_type;
			/* Stream ID is little-endian in RTMP */
			hdr[8] = (uint8_t)(stream_id);
			hdr[9] = (uint8_t)(stream_id >> 8);
			hdr[10] = (uint8_t)(stream_id >> 16);
			hdr[11] = (uint8_t)(stream_id >> 24);
			hdr_len = 12;

			if (timestamp > 0xFFFFFF) {
				if (rtmp_send(ctx, hdr, 12) < 0)
					return -1;
				uint8_t ext[4];
				put_be32(ext, timestamp);
				if (rtmp_send(ctx, ext, 4) < 0)
					return -1;
				hdr_len = 0; /* already sent */
			}
		} else {
			/* fmt=3: no header, just csid */
			hdr[0] = 0xC0 | (csid & 0x3F);
			hdr_len = 1;

			if (timestamp > 0xFFFFFF) {
				if (rtmp_send(ctx, hdr, 1) < 0)
					return -1;
				uint8_t ext[4];
				put_be32(ext, timestamp);
				if (rtmp_send(ctx, ext, 4) < 0)
					return -1;
				hdr_len = 0;
			}
		}

		if (hdr_len > 0) {
			if (rtmp_send(ctx, hdr, hdr_len) < 0)
				return -1;
		}

		if (rtmp_send(ctx, data + offset, payload) < 0)
			return -1;

		offset += payload;
	}

	return 0;
}

/* Send a protocol control message (chunk stream 2, stream 0) */
static int rtmp_send_control(rsp_rtmp_t *ctx, uint8_t msg_type, const uint8_t *data, uint32_t len)
{
	return rtmp_send_message(ctx, RTMP_CSID_CONTROL, msg_type, 0, 0, data, len);
}

/* ── RTMP handshake ── */

static int rtmp_handshake(rsp_rtmp_t *ctx)
{
	uint8_t c0c1[1 + RTMP_HANDSHAKE_SIZE];
	uint8_t s0s1s2[1 + RTMP_HANDSHAKE_SIZE * 2];

	/* C0: version 3 */
	c0c1[0] = 3;

	/* C1: timestamp + zero + random */
	uint32_t ts = (uint32_t)time(NULL);
	put_be32(c0c1 + 1, ts);
	memset(c0c1 + 5, 0, 4); /* zero */

	{
		int urand = open("/dev/urandom", O_RDONLY);
		if (urand >= 0) {
			ssize_t n = read(urand, c0c1 + 9, RTMP_HANDSHAKE_SIZE - 8);
			close(urand);
			if (n < (ssize_t)(RTMP_HANDSHAKE_SIZE - 8))
				memset(c0c1 + 9 + (n > 0 ? n : 0), 0x42, RTMP_HANDSHAKE_SIZE - 8 - (n > 0 ? n : 0));
		} else {
			memset(c0c1 + 9, 0x42, RTMP_HANDSHAKE_SIZE - 8);
		}
	}

	if (rtmp_send(ctx, c0c1, sizeof(c0c1)) < 0) {
		RSS_ERROR("rtmp: handshake C0+C1 send failed");
		return -1;
	}

	/* Read S0 + S1 + S2 */
	if (rtmp_recv_full(ctx, s0s1s2, sizeof(s0s1s2), 10000) < 0) {
		RSS_ERROR("rtmp: handshake S0+S1+S2 recv failed");
		return -1;
	}

	if (s0s1s2[0] != 3) {
		RSS_ERROR("rtmp: server version %d != 3", s0s1s2[0]);
		return -1;
	}

	/* C2: echo S1 with our timestamp */
	uint8_t c2[RTMP_HANDSHAKE_SIZE];
	memcpy(c2, s0s1s2 + 1, RTMP_HANDSHAKE_SIZE);
	put_be32(c2 + 4, ts);

	if (rtmp_send(ctx, c2, sizeof(c2)) < 0) {
		RSS_ERROR("rtmp: handshake C2 send failed");
		return -1;
	}

	ctx->state = RSP_STATE_HANDSHAKE;
	return 0;
}

/* ── RTMP command helpers ── */

static int rtmp_send_connect(rsp_rtmp_t *ctx)
{
	uint8_t buf[2048];
	int off = 0;

	off += amf0_write_string(buf + off, "connect");
	off += amf0_write_number(buf + off, 1.0); /* transaction ID */

	/* Command object */
	buf[off++] = AMF0_OBJECT;
	off += amf0_write_prop_name(buf + off, "app");
	off += amf0_write_string(buf + off, ctx->app);
	off += amf0_write_prop_name(buf + off, "type");
	off += amf0_write_string(buf + off, "nonprivate");
	off += amf0_write_prop_name(buf + off, "flashVer");
	off += amf0_write_string(buf + off, "FMLE/3.0");
	off += amf0_write_prop_name(buf + off, "tcUrl");
	off += amf0_write_string(buf + off, ctx->tc_url);
	off += amf0_write_obj_end(buf + off);

	return rtmp_send_message(ctx, RTMP_CSID_COMMAND, RTMP_MSG_AMF0_CMD, 0, 0, buf,
				 (uint32_t)off);
}

static int rtmp_send_create_stream(rsp_rtmp_t *ctx)
{
	uint8_t buf[64];
	int off = 0;

	off += amf0_write_string(buf + off, "createStream");
	off += amf0_write_number(buf + off, 2.0); /* transaction ID */
	off += amf0_write_null(buf + off);

	return rtmp_send_message(ctx, RTMP_CSID_COMMAND, RTMP_MSG_AMF0_CMD, 0, 0, buf,
				 (uint32_t)off);
}

static int rtmp_send_publish(rsp_rtmp_t *ctx)
{
	uint8_t buf[1024];
	int off = 0;

	off += amf0_write_string(buf + off, "publish");
	off += amf0_write_number(buf + off, 0.0); /* transaction ID */
	off += amf0_write_null(buf + off);
	off += amf0_write_string(buf + off, ctx->stream_key);
	off += amf0_write_string(buf + off, "live");

	return rtmp_send_message(ctx, RTMP_CSID_COMMAND, RTMP_MSG_AMF0_CMD, ctx->stream_id, 0, buf,
				 (uint32_t)off);
}

static int rtmp_send_set_chunk_size(rsp_rtmp_t *ctx, uint32_t size)
{
	uint8_t buf[4];
	put_be32(buf, size);
	ctx->chunk_size = size;
	return rtmp_send_control(ctx, RTMP_MSG_SET_CHUNK_SIZE, buf, 4);
}

static int rtmp_send_window_ack(rsp_rtmp_t *ctx, uint32_t size)
{
	uint8_t buf[4];
	put_be32(buf, size);
	return rtmp_send_control(ctx, RTMP_MSG_WINDOW_ACK_SIZE, buf, 4);
}

/* ── Response processing ── */

/* Per-chunk-stream state for tracking multi-chunk messages */
#define RTMP_MAX_CSID 16
typedef struct {
	uint32_t msg_len;
	uint8_t msg_type;
	uint32_t stream_id;
	uint32_t msg_got; /* bytes received so far for current message */
} rtmp_cs_state_t;

static void rtmp_process_message(rsp_rtmp_t *ctx, uint8_t msg_type, uint32_t msg_len)
{
	switch (msg_type) {
	case RTMP_MSG_SET_CHUNK_SIZE:
		if (msg_len >= 4) {
			ctx->in_chunk_size = get_be32(ctx->recv_buf) & 0x7FFFFFFF;
			RSS_DEBUG("rtmp: server chunk size = %u", ctx->in_chunk_size);
		}
		break;

	case RTMP_MSG_WINDOW_ACK_SIZE:
		if (msg_len >= 4) {
			uint32_t ack = get_be32(ctx->recv_buf);
			RSS_DEBUG("rtmp: window ack size = %u", ack);
		}
		break;

	case RTMP_MSG_SET_PEER_BW:
		rtmp_send_window_ack(ctx, 2500000);
		break;

	case RTMP_MSG_AMF0_CMD:
		if (msg_len > 3 && ctx->recv_buf[0] == AMF0_STRING) {
			uint16_t slen = ((uint16_t)ctx->recv_buf[1] << 8) | ctx->recv_buf[2];
			if (slen > msg_len - 3)
				break;
			RSS_DEBUG("rtmp: AMF0 cmd: %.*s", slen, ctx->recv_buf + 3);

			if (slen == 7 && memcmp(ctx->recv_buf + 3, "_result", 7) == 0) {
				if (ctx->state == RSP_STATE_HANDSHAKE) {
					ctx->state = RSP_STATE_CONNECTING;
				} else if (ctx->state == RSP_STATE_CONNECTING) {
					uint32_t p = 3 + slen;
					if (p < msg_len && ctx->recv_buf[p] == AMF0_NUMBER)
						p += 9;
					if (p < msg_len && ctx->recv_buf[p] == AMF0_NULL)
						p += 1;
					if (p + 9 <= msg_len && ctx->recv_buf[p] == AMF0_NUMBER) {
						union {
							double d;
							uint64_t u;
						} u;
						u.u = 0;
						for (int i = 0; i < 8; i++)
							u.u = (u.u << 8) | ctx->recv_buf[p + 1 + i];
						ctx->stream_id = (uint32_t)u.d;
					}
					ctx->state = RSP_STATE_CONNECTED;
				}
			} else if (slen == 8 && memcmp(ctx->recv_buf + 3, "onStatus", 8) == 0) {
				ctx->state = RSP_STATE_PUBLISHING;
			} else if (slen == 6 && memcmp(ctx->recv_buf + 3, "_error", 6) == 0) {
				RSS_ERROR("rtmp: server returned _error");
				ctx->state = RSP_STATE_ERROR;
			}
		}
		break;

	default:
		RSS_DEBUG("rtmp: unhandled msg type %u len %u", msg_type, msg_len);
		break;
	}
}

/*
 * Read and process server responses until we reach the expected state.
 * Tracks per-chunk-stream headers so fmt=1/2/3 work correctly.
 */
static int rtmp_read_responses(rsp_rtmp_t *ctx, rsp_conn_state_t target_state, int timeout_ms)
{
	int64_t deadline = rss_timestamp_us() + (int64_t)timeout_ms * 1000;
	rtmp_cs_state_t cs[RTMP_MAX_CSID] = {{0}};

	while (ctx->state < target_state && ctx->state != RSP_STATE_ERROR) {
		int remaining = (int)((deadline - rss_timestamp_us()) / 1000);
		if (remaining <= 0) {
			RSS_ERROR("rtmp: timeout waiting for state %d (at %d)", target_state,
				  ctx->state);
			return -1;
		}

		/* Set timeout based on remaining deadline */
		struct timeval tv = {.tv_sec = remaining / 1000,
				     .tv_usec = (remaining % 1000) * 1000};
		setsockopt(ctx->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

		uint8_t basic;
		ssize_t n = rtmp_recv(ctx, &basic, 1);
		if (n <= 0)
			return -1;

		uint8_t fmt = (basic >> 6) & 0x03;
		uint32_t csid = basic & 0x3F;

		/* Extended chunk stream ID */
		if (csid == 0) {
			uint8_t b;
			if (rtmp_recv_full(ctx, &b, 1, remaining) < 0)
				return -1;
			csid = (uint32_t)b + 64;
		} else if (csid == 1) {
			uint8_t b[2];
			if (rtmp_recv_full(ctx, b, 2, remaining) < 0)
				return -1;
			csid = (uint32_t)b[1] * 256 + (uint32_t)b[0] + 64;
		}

		/* Clamp csid to our tracking array */
		uint32_t csi = csid < RTMP_MAX_CSID ? csid : 0;
		rtmp_cs_state_t *c = &cs[csi];

		/* Read message header based on fmt */
		if (fmt == 0) {
			uint8_t mh[11];
			if (rtmp_recv_full(ctx, mh, 11, remaining) < 0)
				return -1;
			uint32_t ts = ((uint32_t)mh[0] << 16) | ((uint32_t)mh[1] << 8) | mh[2];
			c->msg_len = ((uint32_t)mh[3] << 16) | ((uint32_t)mh[4] << 8) | mh[5];
			c->msg_type = mh[6];
			c->stream_id = ((uint32_t)mh[10] << 24) | ((uint32_t)mh[9] << 16) |
				       ((uint32_t)mh[8] << 8) | mh[7];
			c->msg_got = 0;
			if (ts == 0xFFFFFF) {
				uint8_t ext[4];
				if (rtmp_recv_full(ctx, ext, 4, remaining) < 0)
					return -1;
			}
		} else if (fmt == 1) {
			uint8_t mh[7];
			if (rtmp_recv_full(ctx, mh, 7, remaining) < 0)
				return -1;
			uint32_t ts = ((uint32_t)mh[0] << 16) | ((uint32_t)mh[1] << 8) | mh[2];
			c->msg_len = ((uint32_t)mh[3] << 16) | ((uint32_t)mh[4] << 8) | mh[5];
			c->msg_type = mh[6];
			c->msg_got = 0;
			if (ts == 0xFFFFFF) {
				uint8_t ext[4];
				if (rtmp_recv_full(ctx, ext, 4, remaining) < 0)
					return -1;
			}
		} else if (fmt == 2) {
			uint8_t mh[3];
			if (rtmp_recv_full(ctx, mh, 3, remaining) < 0)
				return -1;
			uint32_t ts = ((uint32_t)mh[0] << 16) | ((uint32_t)mh[1] << 8) | mh[2];
			if (ts == 0xFFFFFF) {
				uint8_t ext[4];
				if (rtmp_recv_full(ctx, ext, 4, remaining) < 0)
					return -1;
			}
			/* msg_len, msg_type inherited from previous chunk on this csid */
		}
		/* fmt == 3: everything inherited, continuation of current message */

		if (c->msg_len == 0)
			continue;

		/* Read one chunk of payload */
		uint32_t remaining_msg = c->msg_len - c->msg_got;
		uint32_t chunk =
			remaining_msg > ctx->in_chunk_size ? ctx->in_chunk_size : remaining_msg;

		if (c->msg_got + chunk > ctx->recv_buf_size) {
			/* Message too large — skip this chunk */
			uint8_t tmp[512];
			uint32_t skip = chunk;
			while (skip > 0) {
				uint32_t rd = skip > sizeof(tmp) ? sizeof(tmp) : skip;
				if (rtmp_recv_full(ctx, tmp, rd, remaining) < 0)
					return -1;
				skip -= rd;
			}
			c->msg_got += chunk;
		} else {
			if (rtmp_recv_full(ctx, ctx->recv_buf + c->msg_got, chunk, remaining) < 0)
				return -1;
			c->msg_got += chunk;
		}

		/* Process complete message */
		if (c->msg_got >= c->msg_len)
			rtmp_process_message(ctx, c->msg_type, c->msg_len);
	}

	return ctx->state == RSP_STATE_ERROR ? -1 : 0;
}

/* ── Public API ── */

int rsp_rtmp_parse_url(const char *url, char *host, int host_size, int *port, char *app,
		       int app_size, char *key, int key_size, bool *use_tls)
{
	const char *p = url;

	*use_tls = false;
	if (strncmp(p, "rtmps://", 8) == 0) {
		*use_tls = true;
		p += 8;
	} else if (strncmp(p, "rtmp://", 7) == 0) {
		p += 7;
	} else {
		return -1;
	}

	/* Host (may include port) */
	const char *slash = strchr(p, '/');
	if (!slash)
		return -1;

	const char *colon = memchr(p, ':', (size_t)(slash - p));
	if (colon) {
		int hlen = (int)(colon - p);
		if (hlen >= host_size)
			hlen = host_size - 1;
		memcpy(host, p, (size_t)hlen);
		host[hlen] = '\0';
		*port = atoi(colon + 1);
	} else {
		int hlen = (int)(slash - p);
		if (hlen >= host_size)
			hlen = host_size - 1;
		memcpy(host, p, (size_t)hlen);
		host[hlen] = '\0';
		*port = *use_tls ? RTMPS_DEFAULT_PORT : RTMP_DEFAULT_PORT;
	}

	/* App name (path segment after host) */
	p = slash + 1;
	slash = strchr(p, '/');
	if (!slash)
		return -1;

	int alen = (int)(slash - p);
	if (alen >= app_size)
		alen = app_size - 1;
	memcpy(app, p, (size_t)alen);
	app[alen] = '\0';

	/* Stream key (everything after app/) */
	p = slash + 1;
	rss_strlcpy(key, p, (size_t)key_size);

	return 0;
}

int rsp_rtmp_connect(rsp_rtmp_t *ctx, const char *host, int port, bool use_tls)
{
	/* TCP connect */
	char port_str[8];
	snprintf(port_str, sizeof(port_str), "%d", port);

	struct addrinfo hints = {
		.ai_family = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM,
	};
	struct addrinfo *res;
	int ret = getaddrinfo(host, port_str, &hints, &res);
	if (ret != 0) {
		RSS_ERROR("rtmp: DNS resolve failed for %s: %s", host, gai_strerror(ret));
		return -1;
	}

	ctx->fd = -1;
	for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
		int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;

		struct timeval tv = {.tv_sec = 10};
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

		if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
			ctx->fd = fd;
			break;
		}
		close(fd);
	}
	freeaddrinfo(res);

	if (ctx->fd < 0) {
		RSS_ERROR("rtmp: TCP connect to %s:%d failed", host, port);
		return -1;
	}

	rss_set_tcp_nodelay(ctx->fd);
	if (ctx->tcp_sndbuf > 0) {
		int sndbuf = ctx->tcp_sndbuf;
		setsockopt(ctx->fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
		socklen_t len = sizeof(sndbuf);
		getsockopt(ctx->fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, &len);
		RSS_DEBUG("rtmp: tcp_sndbuf=%d (requested %d)", sndbuf, ctx->tcp_sndbuf);
	}
	ctx->use_tls = use_tls;
	ctx->chunk_size = RTMP_DEFAULT_CHUNK_SZ;
	ctx->in_chunk_size = RTMP_DEFAULT_CHUNK_SZ;
	ctx->stream_id = 0;

	if (!ctx->recv_buf) {
		ctx->recv_buf_size = 4096;
		ctx->recv_buf = malloc(ctx->recv_buf_size);
		if (!ctx->recv_buf) {
			close(ctx->fd);
			ctx->fd = -1;
			return -1;
		}
	}
	if (!ctx->send_buf) {
		ctx->send_buf_size = 65536;
		ctx->send_buf = malloc(ctx->send_buf_size);
		if (!ctx->send_buf) {
			free(ctx->recv_buf);
			ctx->recv_buf = NULL;
			close(ctx->fd);
			ctx->fd = -1;
			return -1;
		}
	}

#ifdef RSS_HAS_TLS
	if (use_tls) {
		if (!ctx->tls_ctx) {
			ctx->tls_ctx = rss_tls_client_init();
			if (!ctx->tls_ctx) {
				RSS_ERROR("rtmp: TLS init failed");
				close(ctx->fd);
				ctx->fd = -1;
				return -1;
			}
		}
		ctx->tls_conn = rss_tls_connect(ctx->tls_ctx, ctx->fd, host, 10000);
		if (!ctx->tls_conn) {
			RSS_ERROR("rtmp: TLS handshake to %s failed", host);
			close(ctx->fd);
			ctx->fd = -1;
			return -1;
		}
		RSS_INFO("rtmp: RTMPS connection established to %s:%d", host, port);
	} else
#else
	if (use_tls) {
		RSS_ERROR("rtmp: RTMPS requested but TLS not compiled in");
		close(ctx->fd);
		ctx->fd = -1;
		return -1;
	}
#endif
	{
		RSS_INFO("rtmp: TCP connection established to %s:%d", host, port);
	}

	/* RTMP handshake */
	if (rtmp_handshake(ctx) < 0) {
		rsp_rtmp_disconnect(ctx);
		return -1;
	}

	/* Set our outgoing chunk size to 4096 */
	if (rtmp_send_set_chunk_size(ctx, RTMP_CHUNK_SZ_OUT) < 0) {
		rsp_rtmp_disconnect(ctx);
		return -1;
	}

	/* Send connect command */
	if (rtmp_send_connect(ctx) < 0) {
		rsp_rtmp_disconnect(ctx);
		return -1;
	}

	/* Wait for connect _result */
	if (rtmp_read_responses(ctx, RSP_STATE_CONNECTING, 10000) < 0) {
		RSS_ERROR("rtmp: connect command failed");
		rsp_rtmp_disconnect(ctx);
		return -1;
	}

	/* Send createStream */
	if (rtmp_send_create_stream(ctx) < 0) {
		rsp_rtmp_disconnect(ctx);
		return -1;
	}

	/* Wait for createStream _result */
	if (rtmp_read_responses(ctx, RSP_STATE_CONNECTED, 10000) < 0) {
		RSS_ERROR("rtmp: createStream failed");
		rsp_rtmp_disconnect(ctx);
		return -1;
	}

	RSS_INFO("rtmp: connected, stream_id=%u", ctx->stream_id);
	return 0;
}

int rsp_rtmp_publish(rsp_rtmp_t *ctx)
{
	if (ctx->state < RSP_STATE_CONNECTED)
		return -1;

	if (rtmp_send_publish(ctx) < 0)
		return -1;

	if (rtmp_read_responses(ctx, RSP_STATE_PUBLISHING, 10000) < 0) {
		RSS_ERROR("rtmp: publish failed");
		return -1;
	}

	RSS_INFO("rtmp: publishing started");
	return 0;
}

int rsp_rtmp_send_video_header(rsp_rtmp_t *ctx, int codec, const uint8_t *sps, uint32_t sps_len,
			       const uint8_t *pps, uint32_t pps_len, const uint8_t *vps,
			       uint32_t vps_len)
{
	if (ctx->state != RSP_STATE_PUBLISHING)
		return -1;

	uint32_t needed = 64 + sps_len + pps_len + vps_len;
	if (needed > ctx->send_buf_size)
		return -1;

	uint8_t *buf = ctx->send_buf;
	uint32_t off = 0;

	if (codec == RSP_FLV_VIDEO_H264) {
		/* FLV video tag: keyframe + AVC */
		buf[off++] = FLV_VIDEO_KEY | FLV_VIDEO_H264;
		buf[off++] = FLV_AVC_SEQUENCE_HDR;
		/* composition time offset = 0 */
		put_be24(buf + off, 0);
		off += 3;

		/* AVCDecoderConfigurationRecord */
		buf[off++] = 1;	     /* version */
		buf[off++] = sps[1]; /* profile */
		buf[off++] = sps[2]; /* compatibility */
		buf[off++] = sps[3]; /* level */
		buf[off++] = 0xFF;   /* lengthSizeMinusOne = 3 (4 bytes) */
		buf[off++] = 0xE1;   /* numSPS = 1 */
		put_be16(buf + off, (uint16_t)sps_len);
		off += 2;
		memcpy(buf + off, sps, sps_len);
		off += sps_len;
		buf[off++] = 1; /* numPPS */
		put_be16(buf + off, (uint16_t)pps_len);
		off += 2;
		memcpy(buf + off, pps, pps_len);
		off += pps_len;
	} else {
		/* Enhanced RTMP: H.265/HEVC via FourCC */
		buf[off++] = FLV_VIDEO_EX_HEADER | FLV_VIDEO_KEY;
		buf[off++] = FLV_PACKET_TYPE_SEQ_START;
		put_be32(buf + off, FLV_VIDEO_FOURCC_HEVC);
		off += 4;

		/* HEVCDecoderConfigurationRecord */
		buf[off++] = 1; /* configurationVersion */
		/* general_profile_space(2) + tier(1) + profile_idc(5) */
		buf[off++] = vps_len > 6 ? vps[6] : 0;
		/* general_profile_compatibility_flags (4 bytes) */
		memset(buf + off, 0, 4);
		off += 4;
		/* general_constraint_indicator_flags (6 bytes) */
		memset(buf + off, 0, 6);
		off += 6;
		/* general_level_idc */
		buf[off++] = sps_len > 12 ? sps[12] : 0;
		/* min_spatial_segmentation_idc (16 bits) */
		put_be16(buf + off, 0xF000);
		off += 2;
		buf[off++] = 0xFC;	/* parallelismType */
		buf[off++] = 0xFC;	/* chromaFormat */
		buf[off++] = 0xF8;	/* bitDepthLumaMinus8 */
		buf[off++] = 0xF8;	/* bitDepthChromaMinus8 */
		put_be16(buf + off, 0); /* avgFrameRate */
		off += 2;
		/* constantFrameRate(2) + numTemporalLayers(3) +
		 * temporalIdNested(1) + lengthSizeMinusOne(2) */
		buf[off++] = 0x0F;
		buf[off++] = 3; /* numOfArrays (VPS+SPS+PPS) */

		/* VPS array */
		buf[off++] = 0x20;	/* NAL type 32 (VPS) */
		put_be16(buf + off, 1); /* numNalus */
		off += 2;
		put_be16(buf + off, (uint16_t)vps_len);
		off += 2;
		memcpy(buf + off, vps, vps_len);
		off += vps_len;

		/* SPS array */
		buf[off++] = 0x21; /* NAL type 33 (SPS) */
		put_be16(buf + off, 1);
		off += 2;
		put_be16(buf + off, (uint16_t)sps_len);
		off += 2;
		memcpy(buf + off, sps, sps_len);
		off += sps_len;

		/* PPS array */
		buf[off++] = 0x22; /* NAL type 34 (PPS) */
		put_be16(buf + off, 1);
		off += 2;
		put_be16(buf + off, (uint16_t)pps_len);
		off += 2;
		memcpy(buf + off, pps, pps_len);
		off += pps_len;
	}

	return rtmp_send_message(ctx, RTMP_CSID_VIDEO, RTMP_MSG_VIDEO, ctx->stream_id, 0, buf, off);
}

int rsp_rtmp_send_video(rsp_rtmp_t *ctx, const uint8_t *avcc, uint32_t len, uint32_t timestamp_ms,
			bool is_key, int codec)
{
	if (ctx->state != RSP_STATE_PUBLISHING)
		return -1;

	uint8_t hdr[16];
	uint32_t hdr_len;

	if (codec == RSP_FLV_VIDEO_H264) {
		hdr[0] = (is_key ? FLV_VIDEO_KEY : FLV_VIDEO_INTER) | FLV_VIDEO_H264;
		hdr[1] = FLV_AVC_NALU;
		put_be24(hdr + 2, 0); /* composition time = 0 (no B-frames) */
		hdr_len = 5;
	} else {
		hdr[0] = FLV_VIDEO_EX_HEADER | (is_key ? FLV_VIDEO_KEY : FLV_VIDEO_INTER);
		hdr[1] = FLV_PACKET_TYPE_CODED_FRAMES;
		put_be32(hdr + 2, FLV_VIDEO_FOURCC_HEVC);
		/* composition time = 0 */
		put_be24(hdr + 6, 0);
		hdr_len = 9;
	}

	/* Send as a single RTMP message: header + AVCC data */
	uint32_t total = hdr_len + len;
	uint8_t chunk_hdr[12];
	uint32_t chunk_sz = ctx->chunk_size;
	uint32_t offset = 0;
	bool first = true;

	while (offset < total) {
		uint32_t payload = total - offset;
		if (payload > chunk_sz)
			payload = chunk_sz;

		int chdr_len;
		if (first) {
			chunk_hdr[0] = RTMP_CSID_VIDEO & 0x3F;
			put_be24(chunk_hdr + 1, timestamp_ms > 0xFFFFFF ? 0xFFFFFF : timestamp_ms);
			put_be24(chunk_hdr + 4, total);
			chunk_hdr[7] = RTMP_MSG_VIDEO;
			chunk_hdr[8] = (uint8_t)(ctx->stream_id);
			chunk_hdr[9] = (uint8_t)(ctx->stream_id >> 8);
			chunk_hdr[10] = (uint8_t)(ctx->stream_id >> 16);
			chunk_hdr[11] = (uint8_t)(ctx->stream_id >> 24);
			chdr_len = 12;
			if (timestamp_ms > 0xFFFFFF) {
				if (rtmp_send(ctx, chunk_hdr, 12) < 0)
					return -1;
				uint8_t ext[4];
				put_be32(ext, timestamp_ms);
				if (rtmp_send(ctx, ext, 4) < 0)
					return -1;
				chdr_len = 0;
			}
			first = false;
		} else {
			chunk_hdr[0] = 0xC0 | (RTMP_CSID_VIDEO & 0x3F);
			chdr_len = 1;
			if (timestamp_ms > 0xFFFFFF) {
				if (rtmp_send(ctx, chunk_hdr, 1) < 0)
					return -1;
				uint8_t ext[4];
				put_be32(ext, timestamp_ms);
				if (rtmp_send(ctx, ext, 4) < 0)
					return -1;
				chdr_len = 0;
			}
		}

		if (chdr_len > 0) {
			if (rtmp_send(ctx, chunk_hdr, chdr_len) < 0)
				return -1;
		}

		/* Send payload: may span hdr and avcc data */
		if (offset < hdr_len) {
			uint32_t from_hdr = hdr_len - offset;
			if (from_hdr > payload)
				from_hdr = payload;
			if (rtmp_send(ctx, hdr + offset, from_hdr) < 0)
				return -1;
			uint32_t from_data = payload - from_hdr;
			if (from_data > 0) {
				if (rtmp_send(ctx, avcc, from_data) < 0)
					return -1;
			}
		} else {
			uint32_t data_off = offset - hdr_len;
			if (rtmp_send(ctx, avcc + data_off, payload) < 0)
				return -1;
		}

		offset += payload;
	}

	return 0;
}

int rsp_rtmp_send_audio_header(rsp_rtmp_t *ctx, uint32_t sample_rate, uint8_t channels)
{
	if (ctx->state != RSP_STATE_PUBLISHING)
		return -1;

	/* AudioSpecificConfig: AAC-LC */
	static const int sr_table[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
				       22050, 16000, 12000, 11025, 8000,  7350};
	int sr_idx = 4;
	for (int i = 0; i < 13; i++) {
		if (sr_table[i] == (int)sample_rate) {
			sr_idx = i;
			break;
		}
	}

	uint16_t asc = (uint16_t)((2 << 11) | (sr_idx << 7) | (channels << 3));

	uint8_t buf[4];
	/* FLV audio tag: AAC, 44kHz, 16-bit, stereo (flags for AAC are fixed) */
	buf[0] = FLV_AUDIO_AAC | FLV_AUDIO_44KHZ | FLV_AUDIO_16BIT | FLV_AUDIO_STEREO;
	buf[1] = FLV_AAC_SEQUENCE_HDR;
	buf[2] = (uint8_t)(asc >> 8);
	buf[3] = (uint8_t)asc;

	return rtmp_send_message(ctx, RTMP_CSID_AUDIO, RTMP_MSG_AUDIO, ctx->stream_id, 0, buf, 4);
}

int rsp_rtmp_send_audio(rsp_rtmp_t *ctx, const uint8_t *data, uint32_t len, uint32_t timestamp_ms)
{
	if (ctx->state != RSP_STATE_PUBLISHING)
		return -1;

	uint8_t hdr[2];
	hdr[0] = FLV_AUDIO_AAC | FLV_AUDIO_44KHZ | FLV_AUDIO_16BIT | FLV_AUDIO_STEREO;
	hdr[1] = FLV_AAC_RAW;

	/* Build single message: 2-byte header + raw AAC AU */
	uint32_t total = 2 + len;

	if (total <= ctx->chunk_size) {
		/* Fast path: fits in one chunk */
		uint8_t chunk_hdr[12];
		chunk_hdr[0] = RTMP_CSID_AUDIO & 0x3F;
		put_be24(chunk_hdr + 1, timestamp_ms > 0xFFFFFF ? 0xFFFFFF : timestamp_ms);
		put_be24(chunk_hdr + 4, total);
		chunk_hdr[7] = RTMP_MSG_AUDIO;
		chunk_hdr[8] = (uint8_t)(ctx->stream_id);
		chunk_hdr[9] = (uint8_t)(ctx->stream_id >> 8);
		chunk_hdr[10] = (uint8_t)(ctx->stream_id >> 16);
		chunk_hdr[11] = (uint8_t)(ctx->stream_id >> 24);

		if (rtmp_send(ctx, chunk_hdr, 12) < 0)
			return -1;
		if (timestamp_ms > 0xFFFFFF) {
			uint8_t ext[4];
			put_be32(ext, timestamp_ms);
			if (rtmp_send(ctx, ext, 4) < 0)
				return -1;
		}
		if (rtmp_send(ctx, hdr, 2) < 0)
			return -1;
		if (rtmp_send(ctx, data, len) < 0)
			return -1;
		return 0;
	}

	/* Slow path: multi-chunk (unlikely for audio) */
	if (total > ctx->send_buf_size)
		return -1;
	uint8_t *tmp = ctx->send_buf;
	memcpy(tmp, hdr, 2);
	memcpy(tmp + 2, data, len);
	return rtmp_send_message(ctx, RTMP_CSID_AUDIO, RTMP_MSG_AUDIO, ctx->stream_id, timestamp_ms,
				 tmp, total);
}

void rsp_rtmp_disconnect(rsp_rtmp_t *ctx)
{
	if (!ctx)
		return;

#ifdef RSS_HAS_TLS
	if (ctx->tls_conn) {
		rss_tls_close(ctx->tls_conn);
		ctx->tls_conn = NULL;
	}
	if (ctx->tls_ctx) {
		rss_tls_client_free(ctx->tls_ctx);
		ctx->tls_ctx = NULL;
	}
#endif

	if (ctx->fd >= 0) {
		close(ctx->fd);
		ctx->fd = -1;
	}

	free(ctx->recv_buf);
	ctx->recv_buf = NULL;
	free(ctx->send_buf);
	ctx->send_buf = NULL;

	ctx->state = RSP_STATE_DISCONNECTED;
	ctx->video_ts_set = false;
	ctx->audio_ts_set = false;
}

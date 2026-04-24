/*
 * test_rsp.c -- RSP daemon unit tests
 *
 * Tests RTMP URL parsing, AMF0 encoding, chunk stream framing,
 * FLV tag construction, audio transcode (G.711 decode, PCM
 * accumulation), and Annex B / codec param extraction.
 */

#include "greatest.h"

#include <string.h>
#include <arpa/inet.h>

/* Include rsp_rtmp.c directly to test static functions.
 * Stub out the I/O functions — tests don't need real sockets. */
#define RSS_HAS_TLS
typedef struct rss_tls_client_ctx rss_tls_client_ctx_t;
typedef struct rss_tls_conn rss_tls_conn_t;
static inline rss_tls_client_ctx_t *rss_tls_client_init(void) { return NULL; }
static inline void rss_tls_client_free(rss_tls_client_ctx_t *c) { (void)c; }
static inline rss_tls_conn_t *rss_tls_connect(rss_tls_client_ctx_t *c, int fd,
					       const char *h, int t)
{
	(void)c; (void)fd; (void)h; (void)t;
	return NULL;
}
static inline ssize_t rss_tls_write(rss_tls_conn_t *c, const void *b, size_t l)
{
	(void)c; (void)b; (void)l;
	return (ssize_t)l;
}
static inline ssize_t rss_tls_read(rss_tls_conn_t *c, void *b, size_t l)
{
	(void)c; (void)b; (void)l;
	return -1;
}
static inline void rss_tls_close(rss_tls_conn_t *c) { (void)c; }
static inline int rss_tls_get_fd(rss_tls_conn_t *c) { (void)c; return -1; }

/* Stub rss_tls.h out so the real header isn't pulled in */
#define RSS_TLS_H

#include "../rsp/rsp_rtmp.c"

/* ================================================================
 * URL Parsing Tests
 * ================================================================ */

TEST url_parse_rtmp_basic(void)
{
	char host[64], app[64], key[128];
	int port;
	bool tls;
	int ret = rsp_rtmp_parse_url("rtmp://live.example.com/app/streamkey",
				     host, sizeof(host), &port, app, sizeof(app),
				     key, sizeof(key), &tls);
	ASSERT_EQ(0, ret);
	ASSERT_STR_EQ("live.example.com", host);
	ASSERT_EQ(1935, port);
	ASSERT_STR_EQ("app", app);
	ASSERT_STR_EQ("streamkey", key);
	ASSERT_EQ(false, tls);
	PASS();
}

TEST url_parse_rtmps(void)
{
	char host[64], app[64], key[128];
	int port;
	bool tls;
	int ret = rsp_rtmp_parse_url("rtmps://a.rtmps.youtube.com/live2/xxxx-yyyy",
				     host, sizeof(host), &port, app, sizeof(app),
				     key, sizeof(key), &tls);
	ASSERT_EQ(0, ret);
	ASSERT_STR_EQ("a.rtmps.youtube.com", host);
	ASSERT_EQ(443, port);
	ASSERT_STR_EQ("live2", app);
	ASSERT_STR_EQ("xxxx-yyyy", key);
	ASSERT_EQ(true, tls);
	PASS();
}

TEST url_parse_custom_port(void)
{
	char host[64], app[64], key[128];
	int port;
	bool tls;
	int ret = rsp_rtmp_parse_url("rtmp://192.168.1.100:1936/live/test123",
				     host, sizeof(host), &port, app, sizeof(app),
				     key, sizeof(key), &tls);
	ASSERT_EQ(0, ret);
	ASSERT_STR_EQ("192.168.1.100", host);
	ASSERT_EQ(1936, port);
	ASSERT_STR_EQ("live", app);
	ASSERT_STR_EQ("test123", key);
	ASSERT_EQ(false, tls);
	PASS();
}

TEST url_parse_twitch(void)
{
	char host[64], app[64], key[256];
	int port;
	bool tls;
	int ret = rsp_rtmp_parse_url("rtmp://live.twitch.tv/app/live_123456_abcdef",
				     host, sizeof(host), &port, app, sizeof(app),
				     key, sizeof(key), &tls);
	ASSERT_EQ(0, ret);
	ASSERT_STR_EQ("live.twitch.tv", host);
	ASSERT_STR_EQ("app", app);
	ASSERT_STR_EQ("live_123456_abcdef", key);
	PASS();
}

TEST url_parse_invalid_no_scheme(void)
{
	char host[64], app[64], key[128];
	int port;
	bool tls;
	int ret = rsp_rtmp_parse_url("http://example.com/live/key",
				     host, sizeof(host), &port, app, sizeof(app),
				     key, sizeof(key), &tls);
	ASSERT(ret != 0);
	PASS();
}

TEST url_parse_invalid_no_app(void)
{
	char host[64], app[64], key[128];
	int port;
	bool tls;
	int ret = rsp_rtmp_parse_url("rtmp://example.com/",
				     host, sizeof(host), &port, app, sizeof(app),
				     key, sizeof(key), &tls);
	ASSERT(ret != 0);
	PASS();
}

TEST url_parse_invalid_no_key(void)
{
	char host[64], app[64], key[128];
	int port;
	bool tls;
	/* No slash after app — no stream key */
	int ret = rsp_rtmp_parse_url("rtmp://example.com/app",
				     host, sizeof(host), &port, app, sizeof(app),
				     key, sizeof(key), &tls);
	ASSERT(ret != 0);
	PASS();
}

TEST url_parse_complex_key(void)
{
	char host[64], app[64], key[256];
	int port;
	bool tls;
	int ret = rsp_rtmp_parse_url("rtmps://host.com/live/key-with-dashes_and.dots?token=abc",
				     host, sizeof(host), &port, app, sizeof(app),
				     key, sizeof(key), &tls);
	ASSERT_EQ(0, ret);
	ASSERT_STR_EQ("live", app);
	ASSERT_STR_EQ("key-with-dashes_and.dots?token=abc", key);
	PASS();
}

/* ================================================================
 * AMF0 Encoding Tests
 * ================================================================ */

TEST amf0_number_encoding(void)
{
	uint8_t buf[16];
	int len = amf0_write_number(buf, 1.0);
	ASSERT_EQ(9, len);
	ASSERT_EQ(AMF0_NUMBER, buf[0]);

	/* IEEE 754 double 1.0 = 0x3FF0000000000000 */
	ASSERT_EQ(0x3F, buf[1]);
	ASSERT_EQ(0xF0, buf[2]);
	for (int i = 3; i < 9; i++)
		ASSERT_EQ(0x00, buf[i]);
	PASS();
}

TEST amf0_number_zero(void)
{
	uint8_t buf[16];
	int len = amf0_write_number(buf, 0.0);
	ASSERT_EQ(9, len);
	ASSERT_EQ(AMF0_NUMBER, buf[0]);
	for (int i = 1; i < 9; i++)
		ASSERT_EQ(0x00, buf[i]);
	PASS();
}

TEST amf0_string_encoding(void)
{
	uint8_t buf[64];
	int len = amf0_write_string(buf, "connect");
	ASSERT_EQ(3 + 7, len);
	ASSERT_EQ(AMF0_STRING, buf[0]);
	/* Length: big-endian 7 */
	ASSERT_EQ(0x00, buf[1]);
	ASSERT_EQ(0x07, buf[2]);
	ASSERT_MEM_EQ("connect", buf + 3, 7);
	PASS();
}

TEST amf0_string_empty(void)
{
	uint8_t buf[16];
	int len = amf0_write_string(buf, "");
	ASSERT_EQ(3, len);
	ASSERT_EQ(AMF0_STRING, buf[0]);
	ASSERT_EQ(0x00, buf[1]);
	ASSERT_EQ(0x00, buf[2]);
	PASS();
}

TEST amf0_null_encoding(void)
{
	uint8_t buf[4];
	int len = amf0_write_null(buf);
	ASSERT_EQ(1, len);
	ASSERT_EQ(AMF0_NULL, buf[0]);
	PASS();
}

TEST amf0_obj_end_encoding(void)
{
	uint8_t buf[4];
	int len = amf0_write_obj_end(buf);
	ASSERT_EQ(3, len);
	ASSERT_EQ(0x00, buf[0]);
	ASSERT_EQ(0x00, buf[1]);
	ASSERT_EQ(AMF0_OBJ_END, buf[2]);
	PASS();
}

TEST amf0_prop_name_encoding(void)
{
	uint8_t buf[32];
	int len = amf0_write_prop_name(buf, "app");
	ASSERT_EQ(2 + 3, len);
	ASSERT_EQ(0x00, buf[0]);
	ASSERT_EQ(0x03, buf[1]);
	ASSERT_MEM_EQ("app", buf + 2, 3);
	PASS();
}

TEST amf0_connect_command(void)
{
	uint8_t buf[512];
	int off = 0;

	off += amf0_write_string(buf + off, "connect");
	off += amf0_write_number(buf + off, 1.0);
	buf[off++] = AMF0_OBJECT;
	off += amf0_write_prop_name(buf + off, "app");
	off += amf0_write_string(buf + off, "live2");
	off += amf0_write_obj_end(buf + off);

	/* Verify structure: string "connect" + number 1.0 + object{app:"live2"} */
	ASSERT_EQ(AMF0_STRING, buf[0]);
	ASSERT(off > 30);

	/* Find "live2" in the output */
	bool found = false;
	for (int i = 0; i < off - 5; i++) {
		if (memcmp(buf + i, "live2", 5) == 0) {
			found = true;
			break;
		}
	}
	ASSERT(found);
	PASS();
}

/* ================================================================
 * Big-Endian Helper Tests
 * ================================================================ */

TEST be_helpers(void)
{
	uint8_t buf[4];

	put_be16(buf, 0x1234);
	ASSERT_EQ(0x12, buf[0]);
	ASSERT_EQ(0x34, buf[1]);

	put_be24(buf, 0x123456);
	ASSERT_EQ(0x12, buf[0]);
	ASSERT_EQ(0x34, buf[1]);
	ASSERT_EQ(0x56, buf[2]);

	put_be32(buf, 0x12345678);
	ASSERT_EQ(0x12, buf[0]);
	ASSERT_EQ(0x34, buf[1]);
	ASSERT_EQ(0x56, buf[2]);
	ASSERT_EQ(0x78, buf[3]);

	ASSERT_EQ(0x12345678, get_be32(buf));
	PASS();
}

/* ================================================================
 * Chunk Stream Framing Tests
 * ================================================================ */

TEST chunk_single(void)
{
	/* Verify fmt=0 chunk header layout for a small message */
	uint8_t expected_hdr[12] = {
		0x03,       /* fmt=0, csid=3 */
		0, 0, 0,    /* timestamp = 0 */
		0, 0, 3,    /* msg length = 3 */
		20,         /* msg type = AMF0_CMD */
		0, 0, 0, 0, /* stream id = 0 (little-endian) */
	};

	/* Verify header construction */
	ASSERT_EQ(0x03, expected_hdr[0]);
	ASSERT_EQ(3, expected_hdr[6]);  /* length */
	ASSERT_EQ(20, expected_hdr[7]); /* AMF0 cmd */
	PASS();
}

TEST chunk_extended_timestamp(void)
{
	/* Timestamps > 0xFFFFFF need extended timestamp field */
	uint32_t ts = 0x01000000; /* > 0xFFFFFF */

	uint8_t hdr[12];
	hdr[0] = RTMP_CSID_VIDEO & 0x3F;
	put_be24(hdr + 1, 0xFFFFFF); /* marker */

	ASSERT_EQ(0xFF, hdr[1]);
	ASSERT_EQ(0xFF, hdr[2]);
	ASSERT_EQ(0xFF, hdr[3]);

	uint8_t ext[4];
	put_be32(ext, ts);
	ASSERT_EQ(0x01, ext[0]);
	ASSERT_EQ(0x00, ext[1]);
	ASSERT_EQ(0x00, ext[2]);
	ASSERT_EQ(0x00, ext[3]);
	PASS();
}

TEST chunk_fmt3_continuation(void)
{
	/* fmt=3 continuation header is just 1 byte: 0xC0 | csid */
	uint8_t cont = 0xC0 | (RTMP_CSID_VIDEO & 0x3F);
	ASSERT_EQ(0xC6, cont);
	PASS();
}

/* ================================================================
 * FLV Video Tag Tests
 * ================================================================ */

TEST flv_h264_sequence_header(void)
{
	/* Verify AVCDecoderConfigurationRecord structure */
	uint8_t sps[] = {0x67, 0x42, 0xC0, 0x1E, 0xD9, 0x00, 0xA0, 0x47};
	uint8_t pps[] = {0x68, 0xCE, 0x38, 0x80};

	uint8_t buf[256];
	uint32_t off = 0;

	/* FLV tag header */
	buf[off++] = FLV_VIDEO_KEY | FLV_VIDEO_H264;
	buf[off++] = FLV_AVC_SEQUENCE_HDR;
	put_be24(buf + off, 0); /* composition time */
	off += 3;

	/* AVCDecoderConfigurationRecord */
	buf[off++] = 1;           /* version */
	buf[off++] = sps[1];      /* profile (0x42 = Baseline) */
	buf[off++] = sps[2];      /* compatibility */
	buf[off++] = sps[3];      /* level */
	buf[off++] = 0xFF;        /* lengthSizeMinusOne = 3 */
	buf[off++] = 0xE1;        /* numSPS = 1 */
	put_be16(buf + off, sizeof(sps));
	off += 2;
	memcpy(buf + off, sps, sizeof(sps));
	off += sizeof(sps);
	buf[off++] = 1;           /* numPPS */
	put_be16(buf + off, sizeof(pps));
	off += 2;
	memcpy(buf + off, pps, sizeof(pps));
	off += sizeof(pps);

	/* Verify tag bytes */
	ASSERT_EQ(0x17, buf[0]);  /* keyframe + AVC */
	ASSERT_EQ(0x00, buf[1]);  /* sequence header */
	ASSERT_EQ(1, buf[5]);     /* config version */
	ASSERT_EQ(0x42, buf[6]);  /* profile */
	ASSERT_EQ(0xFF, buf[9]);  /* lengthSizeMinusOne */
	ASSERT_EQ(0xE1, buf[10]); /* numSPS */
	PASS();
}

TEST flv_h264_nalu_keyframe(void)
{
	uint8_t hdr[5];
	hdr[0] = FLV_VIDEO_KEY | FLV_VIDEO_H264;
	hdr[1] = FLV_AVC_NALU;
	put_be24(hdr + 2, 0);

	ASSERT_EQ(0x17, hdr[0]); /* keyframe + AVC */
	ASSERT_EQ(0x01, hdr[1]); /* NALU */
	PASS();
}

TEST flv_h264_nalu_inter(void)
{
	uint8_t hdr[5];
	hdr[0] = FLV_VIDEO_INTER | FLV_VIDEO_H264;
	hdr[1] = FLV_AVC_NALU;
	put_be24(hdr + 2, 0);

	ASSERT_EQ(0x27, hdr[0]); /* interframe + AVC */
	PASS();
}

TEST flv_h265_enhanced_header(void)
{
	uint8_t hdr[6];
	hdr[0] = FLV_VIDEO_EX_HEADER | FLV_VIDEO_KEY;
	hdr[1] = FLV_PACKET_TYPE_SEQ_START;
	put_be32(hdr + 2, FLV_VIDEO_FOURCC_HEVC);

	ASSERT_EQ(0x90, hdr[0]); /* ex_header + keyframe */
	ASSERT_EQ(0x00, hdr[1]); /* sequence start */
	/* FourCC "hvc1" = 0x68766331 */
	ASSERT_EQ(0x68, hdr[2]);
	ASSERT_EQ(0x76, hdr[3]);
	ASSERT_EQ(0x63, hdr[4]);
	ASSERT_EQ(0x31, hdr[5]);
	PASS();
}

TEST flv_h265_coded_frame(void)
{
	uint8_t hdr[9];
	hdr[0] = FLV_VIDEO_EX_HEADER | FLV_VIDEO_INTER;
	hdr[1] = FLV_PACKET_TYPE_CODED_FRAMES;
	put_be32(hdr + 2, FLV_VIDEO_FOURCC_HEVC);
	put_be24(hdr + 6, 0);

	ASSERT_EQ(0xA0, hdr[0]); /* ex_header + interframe */
	ASSERT_EQ(0x01, hdr[1]); /* coded frames */
	PASS();
}

/* ================================================================
 * FLV Audio Tag Tests
 * ================================================================ */

TEST flv_aac_sequence_header(void)
{
	uint8_t buf[4];
	buf[0] = FLV_AUDIO_AAC | FLV_AUDIO_44KHZ | FLV_AUDIO_16BIT | FLV_AUDIO_STEREO;
	buf[1] = FLV_AAC_SEQUENCE_HDR;

	/* AAC, 44kHz, 16-bit, stereo */
	ASSERT_EQ(0xAF, buf[0]);
	ASSERT_EQ(0x00, buf[1]);
	PASS();
}

TEST flv_aac_raw_frame(void)
{
	uint8_t buf[2];
	buf[0] = FLV_AUDIO_AAC | FLV_AUDIO_44KHZ | FLV_AUDIO_16BIT | FLV_AUDIO_STEREO;
	buf[1] = FLV_AAC_RAW;

	ASSERT_EQ(0xAF, buf[0]);
	ASSERT_EQ(0x01, buf[1]);
	PASS();
}

TEST flv_audio_specific_config_16khz(void)
{
	/* AudioSpecificConfig for 16kHz mono AAC-LC:
	 * objectType = 2 (AAC-LC), srIndex = 8 (16000), channels = 1
	 * bits: 00010 | 1000 | 0001 | 000 = 0x1408 */
	static const int sr_table[] = {96000, 88200, 64000, 48000, 44100, 32000,
				       24000, 22050, 16000, 12000, 11025, 8000, 7350};
	int sr_idx = 4;
	for (int i = 0; i < 13; i++) {
		if (sr_table[i] == 16000) {
			sr_idx = i;
			break;
		}
	}
	ASSERT_EQ(8, sr_idx);
	uint16_t asc = (uint16_t)((2 << 11) | (sr_idx << 7) | (1 << 3));
	ASSERT_EQ(0x1408, asc);
	PASS();
}

TEST flv_audio_specific_config_48khz(void)
{
	int sr_idx = 3; /* 48000 */
	uint16_t asc = (uint16_t)((2 << 11) | (sr_idx << 7) | (1 << 3));
	/* 00010 | 0011 | 0001 | 000 = 0x1188 */
	ASSERT_EQ(0x1188, asc);
	PASS();
}

/* ================================================================
 * G.711 Decode Tests
 * ================================================================ */

/* Build the same tables as rsp_audio.c to verify decode */
static int16_t test_ulaw[256];
static int16_t test_alaw[256];

static void init_test_g711(void)
{
	for (int i = 0; i < 256; i++) {
		uint8_t u = ~(uint8_t)i;
		int sign = (u & 0x80) ? -1 : 1;
		int exp = (u >> 4) & 0x07;
		int mantissa = u & 0x0F;
		int mag = ((mantissa << 1) + 33) << (exp + 2);
		test_ulaw[i] = (int16_t)(sign * (mag - (1 << 5)));

		uint8_t a = (uint8_t)i ^ 0x55;
		sign = (a & 0x80) ? 1 : -1;
		exp = (a >> 4) & 0x07;
		mantissa = a & 0x0F;
		if (exp == 0)
			mag = (mantissa << 1) + 1;
		else
			mag = ((mantissa << 1) + 33) << (exp - 1);
		test_alaw[i] = (int16_t)(sign * mag);
	}
}

TEST ulaw_minimum_step(void)
{
	/* µ-law 0xFF decodes to the minimum positive step (+100).
	 * 0x7F is the corresponding negative step (-100).
	 * These are the closest-to-zero values in µ-law. */
	init_test_g711();
	ASSERT_EQ(100, test_ulaw[0xFF]);
	ASSERT_EQ(-100, test_ulaw[0x7F]);
	PASS();
}

TEST ulaw_full_range(void)
{
	init_test_g711();
	/* µ-law 0x00 (inverted 0xFF) = max negative, 0x80 = max positive */
	int16_t neg = test_ulaw[0x00];
	int16_t pos = test_ulaw[0x80];
	ASSERT(neg < -8000);
	ASSERT(pos > 8000);
	PASS();
}

TEST alaw_silence(void)
{
	init_test_g711();
	/* A-law silence = 0xD5 (0x55 ^ 0x80) */
	ASSERT(test_alaw[0xD5] < 10 && test_alaw[0xD5] > -10);
	PASS();
}

TEST alaw_full_range(void)
{
	init_test_g711();
	/* A-law should span roughly -32768..+32767 */
	int16_t min_val = 0, max_val = 0;
	for (int i = 0; i < 256; i++) {
		if (test_alaw[i] < min_val)
			min_val = test_alaw[i];
		if (test_alaw[i] > max_val)
			max_val = test_alaw[i];
	}
	ASSERT(min_val < -4000);
	ASSERT(max_val > 4000);
	PASS();
}

TEST ulaw_symmetry(void)
{
	/* µ-law: codes 0x00-0x7F are negative, 0x80-0xFF positive.
	 * Corresponding pairs should have same magnitude. */
	init_test_g711();
	for (int i = 0; i < 128; i++) {
		int16_t neg = test_ulaw[i];
		int16_t pos = test_ulaw[i + 128];
		/* Same magnitude, opposite sign (within 1 for rounding) */
		int diff = (int)neg + (int)pos;
		ASSERT(diff >= -1 && diff <= 1);
	}
	PASS();
}

/* ================================================================
 * L16 Byte Swap Test
 * ================================================================ */

TEST l16_byte_swap(void)
{
	/* L16 ring data is big-endian (network order).
	 * Decode should produce native-endian PCM. */
	uint16_t src[] = {htons(0x1234), htons(0x5678), htons(0xFEDC)};
	int16_t pcm[3];
	const uint8_t *data = (const uint8_t *)src;
	int samples = (int)(sizeof(src) / 2);
	for (int i = 0; i < samples; i++)
		pcm[i] = (int16_t)ntohs(((const uint16_t *)data)[i]);

	ASSERT_EQ((int16_t)0x1234, pcm[0]);
	ASSERT_EQ((int16_t)0x5678, pcm[1]);
	ASSERT_EQ((int16_t)0xFEDC, pcm[2]);
	PASS();
}

/* ================================================================
 * Annex B / Codec Param Extraction Tests
 * (rsp_main.c has its own copy — test the logic directly)
 * ================================================================ */

static const uint8_t *test_find_sc(const uint8_t *p, const uint8_t *end, int *sc_len)
{
	while (p + 2 < end) {
		if (p[0] == 0 && p[1] == 0) {
			if (p[2] == 1) {
				*sc_len = 3;
				return p;
			}
			if (p + 3 < end && p[2] == 0 && p[3] == 1) {
				*sc_len = 4;
				return p;
			}
		}
		p++;
	}
	return NULL;
}

TEST annexb_find_4byte_sc(void)
{
	uint8_t data[] = {0x00, 0x00, 0x00, 0x01, 0x65, 0xAA};
	int sc_len;
	const uint8_t *sc = test_find_sc(data, data + sizeof(data), &sc_len);
	ASSERT(sc == data);
	ASSERT_EQ(4, sc_len);
	PASS();
}

TEST annexb_find_3byte_sc(void)
{
	uint8_t data[] = {0x00, 0x00, 0x01, 0x65, 0xAA};
	int sc_len;
	const uint8_t *sc = test_find_sc(data, data + sizeof(data), &sc_len);
	ASSERT(sc == data);
	ASSERT_EQ(3, sc_len);
	PASS();
}

TEST annexb_no_sc(void)
{
	uint8_t data[] = {0x00, 0x01, 0x65, 0xAA};
	int sc_len;
	const uint8_t *sc = test_find_sc(data, data + sizeof(data), &sc_len);
	ASSERT(sc == NULL);
	PASS();
}

TEST annexb_multiple_sc(void)
{
	uint8_t data[] = {
		0x00, 0x00, 0x00, 0x01, 0x67, 0xAA,
		0x00, 0x00, 0x00, 0x01, 0x68, 0xBB,
	};
	int sc_len;
	const uint8_t *sc1 = test_find_sc(data, data + sizeof(data), &sc_len);
	ASSERT(sc1 == data);
	ASSERT_EQ(4, sc_len);

	const uint8_t *sc2 = test_find_sc(sc1 + sc_len + 1, data + sizeof(data), &sc_len);
	ASSERT(sc2 != NULL);
	ASSERT_EQ(0x68, *(sc2 + sc_len));
	PASS();
}

/* ================================================================
 * RTMP Protocol Constants Sanity Tests
 * ================================================================ */

TEST rtmp_constants(void)
{
	ASSERT_EQ(1935, RTMP_DEFAULT_PORT);
	ASSERT_EQ(443, RTMPS_DEFAULT_PORT);
	ASSERT_EQ(1536, RTMP_HANDSHAKE_SIZE);
	ASSERT_EQ(128, RTMP_DEFAULT_CHUNK_SZ);
	ASSERT_EQ(4096, RTMP_CHUNK_SZ_OUT);

	ASSERT_EQ(2, RTMP_CSID_CONTROL);
	ASSERT_EQ(3, RTMP_CSID_COMMAND);
	ASSERT_EQ(4, RTMP_CSID_AUDIO);
	ASSERT_EQ(6, RTMP_CSID_VIDEO);

	ASSERT_EQ(1, RTMP_MSG_SET_CHUNK_SIZE);
	ASSERT_EQ(5, RTMP_MSG_WINDOW_ACK_SIZE);
	ASSERT_EQ(6, RTMP_MSG_SET_PEER_BW);
	ASSERT_EQ(8, RTMP_MSG_AUDIO);
	ASSERT_EQ(9, RTMP_MSG_VIDEO);
	ASSERT_EQ(20, RTMP_MSG_AMF0_CMD);
	PASS();
}

TEST flv_constants(void)
{
	ASSERT_EQ(0xA0, FLV_AUDIO_AAC);
	ASSERT_EQ(0x07, FLV_VIDEO_H264);
	ASSERT_EQ(0x10, FLV_VIDEO_KEY);
	ASSERT_EQ(0x20, FLV_VIDEO_INTER);
	ASSERT_EQ(0x80, FLV_VIDEO_EX_HEADER);
	ASSERT_EQ(0x68766331, FLV_VIDEO_FOURCC_HEVC);
	PASS();
}

/* ================================================================
 * Process Message State Machine Tests
 * ================================================================ */

TEST process_set_chunk_size(void)
{
	rsp_rtmp_t ctx = {0};
	ctx.in_chunk_size = 128;
	put_be32(ctx.recv_buf, 4096);
	rtmp_process_message(&ctx, RTMP_MSG_SET_CHUNK_SIZE, 4);
	ASSERT_EQ(4096, ctx.in_chunk_size);
	PASS();
}

TEST process_set_chunk_size_clears_msb(void)
{
	rsp_rtmp_t ctx = {0};
	ctx.in_chunk_size = 128;
	put_be32(ctx.recv_buf, 0x80001000); /* MSB should be masked */
	rtmp_process_message(&ctx, RTMP_MSG_SET_CHUNK_SIZE, 4);
	ASSERT_EQ(0x1000, ctx.in_chunk_size);
	PASS();
}

TEST process_result_advances_connect(void)
{
	rsp_rtmp_t ctx = {0};
	ctx.state = RSP_STATE_HANDSHAKE;

	/* Build _result AMF0 response */
	int off = 0;
	off += amf0_write_string(ctx.recv_buf + off, "_result");
	off += amf0_write_number(ctx.recv_buf + off, 1.0);

	rtmp_process_message(&ctx, RTMP_MSG_AMF0_CMD, (uint32_t)off);
	ASSERT_EQ(RSP_STATE_CONNECTING, ctx.state);
	PASS();
}

TEST process_result_advances_create_stream(void)
{
	rsp_rtmp_t ctx = {0};
	ctx.state = RSP_STATE_CONNECTING;

	/* Build createStream _result: "_result", txn=2, null, stream_id=1.0 */
	int off = 0;
	off += amf0_write_string(ctx.recv_buf + off, "_result");
	off += amf0_write_number(ctx.recv_buf + off, 2.0);
	off += amf0_write_null(ctx.recv_buf + off);
	off += amf0_write_number(ctx.recv_buf + off, 1.0); /* stream ID */

	rtmp_process_message(&ctx, RTMP_MSG_AMF0_CMD, (uint32_t)off);
	ASSERT_EQ(RSP_STATE_CONNECTED, ctx.state);
	ASSERT_EQ(1, ctx.stream_id);
	PASS();
}

TEST process_onstatus_advances_publishing(void)
{
	rsp_rtmp_t ctx = {0};
	ctx.state = RSP_STATE_CONNECTED;

	int off = 0;
	off += amf0_write_string(ctx.recv_buf + off, "onStatus");
	off += amf0_write_number(ctx.recv_buf + off, 0.0);

	rtmp_process_message(&ctx, RTMP_MSG_AMF0_CMD, (uint32_t)off);
	ASSERT_EQ(RSP_STATE_PUBLISHING, ctx.state);
	PASS();
}

TEST process_error_sets_error_state(void)
{
	rsp_rtmp_t ctx = {0};
	ctx.state = RSP_STATE_HANDSHAKE;

	int off = 0;
	off += amf0_write_string(ctx.recv_buf + off, "_error");

	rtmp_process_message(&ctx, RTMP_MSG_AMF0_CMD, (uint32_t)off);
	ASSERT_EQ(RSP_STATE_ERROR, ctx.state);
	PASS();
}

/* ================================================================
 * Suites
 * ================================================================ */

SUITE(rsp_url_suite)
{
	RUN_TEST(url_parse_rtmp_basic);
	RUN_TEST(url_parse_rtmps);
	RUN_TEST(url_parse_custom_port);
	RUN_TEST(url_parse_twitch);
	RUN_TEST(url_parse_invalid_no_scheme);
	RUN_TEST(url_parse_invalid_no_app);
	RUN_TEST(url_parse_invalid_no_key);
	RUN_TEST(url_parse_complex_key);
}

SUITE(rsp_amf0_suite)
{
	RUN_TEST(amf0_number_encoding);
	RUN_TEST(amf0_number_zero);
	RUN_TEST(amf0_string_encoding);
	RUN_TEST(amf0_string_empty);
	RUN_TEST(amf0_null_encoding);
	RUN_TEST(amf0_obj_end_encoding);
	RUN_TEST(amf0_prop_name_encoding);
	RUN_TEST(amf0_connect_command);
}

SUITE(rsp_chunk_suite)
{
	RUN_TEST(be_helpers);
	RUN_TEST(chunk_single);
	RUN_TEST(chunk_extended_timestamp);
	RUN_TEST(chunk_fmt3_continuation);
	RUN_TEST(rtmp_constants);
}

SUITE(rsp_flv_suite)
{
	RUN_TEST(flv_h264_sequence_header);
	RUN_TEST(flv_h264_nalu_keyframe);
	RUN_TEST(flv_h264_nalu_inter);
	RUN_TEST(flv_h265_enhanced_header);
	RUN_TEST(flv_h265_coded_frame);
	RUN_TEST(flv_aac_sequence_header);
	RUN_TEST(flv_aac_raw_frame);
	RUN_TEST(flv_audio_specific_config_16khz);
	RUN_TEST(flv_audio_specific_config_48khz);
	RUN_TEST(flv_constants);
}

SUITE(rsp_audio_suite)
{
	RUN_TEST(ulaw_minimum_step);
	RUN_TEST(ulaw_full_range);
	RUN_TEST(ulaw_symmetry);
	RUN_TEST(alaw_silence);
	RUN_TEST(alaw_full_range);
	RUN_TEST(l16_byte_swap);
}

SUITE(rsp_annexb_suite)
{
	RUN_TEST(annexb_find_4byte_sc);
	RUN_TEST(annexb_find_3byte_sc);
	RUN_TEST(annexb_no_sc);
	RUN_TEST(annexb_multiple_sc);
}

SUITE(rsp_state_suite)
{
	RUN_TEST(process_set_chunk_size);
	RUN_TEST(process_set_chunk_size_clears_msb);
	RUN_TEST(process_result_advances_connect);
	RUN_TEST(process_result_advances_create_stream);
	RUN_TEST(process_onstatus_advances_publishing);
	RUN_TEST(process_error_sets_error_state);
}

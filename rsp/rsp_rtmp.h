/*
 * rsp_rtmp.h -- RTMP client for stream push
 *
 * Implements the subset of RTMP needed for publishing:
 * handshake, connect, createStream, publish, and A/V data.
 * Supports RTMP (plain TCP) and RTMPS (TLS).
 */

#ifndef RSP_RTMP_H
#define RSP_RTMP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef RSS_HAS_TLS
#include <rss_tls.h>
#endif

/* RTMP connection state */
typedef enum {
	RSP_STATE_DISCONNECTED = 0,
	RSP_STATE_HANDSHAKE,
	RSP_STATE_CONNECTING,
	RSP_STATE_CONNECTED,
	RSP_STATE_PUBLISHING,
	RSP_STATE_ERROR,
} rsp_conn_state_t;

/* Video codec for FLV tags */
#define RSP_FLV_VIDEO_H264 0
#define RSP_FLV_VIDEO_H265 1

/* RTMP connection context */
typedef struct {
	int fd;
	rsp_conn_state_t state;
	bool use_tls;

#ifdef RSS_HAS_TLS
	rss_tls_client_ctx_t *tls_ctx;
	rss_tls_conn_t *tls_conn;
#endif

	/* RTMP chunk state */
	uint32_t chunk_size;	/* outgoing chunk size (default 128) */
	uint32_t in_chunk_size; /* incoming chunk size */
	uint32_t stream_id;	/* server-assigned stream ID */

	/* Stream key, app, and tcUrl */
	char app[256];
	char stream_key[512];
	char tc_url[768];

	/* Timestamp tracking */
	uint32_t video_ts_base;
	uint32_t audio_ts_base;
	bool video_ts_set;
	bool audio_ts_set;

	/* I/O buffers (heap-allocated) */
	uint8_t *recv_buf;
	uint32_t recv_buf_size;
	uint8_t *send_buf;
	uint32_t send_buf_size;
} rsp_rtmp_t;

/*
 * Parse an RTMP(S) URL into host, port, app, and stream key.
 * URL format: rtmp[s]://host[:port]/app/stream_key
 * Returns 0 on success.
 */
int rsp_rtmp_parse_url(const char *url, char *host, int host_size, int *port, char *app,
		       int app_size, char *key, int key_size, bool *use_tls);

/*
 * Connect to an RTMP server and begin publishing.
 * Performs TCP connect, optional TLS, RTMP handshake, connect,
 * createStream, and publish in sequence.
 * Returns 0 on success.
 */
int rsp_rtmp_connect(rsp_rtmp_t *ctx, const char *host, int port, bool use_tls);

/*
 * Send the RTMP publish command after connect.
 */
int rsp_rtmp_publish(rsp_rtmp_t *ctx);

/*
 * Send video sequence header (SPS/PPS for H.264, VPS/SPS/PPS for H.265).
 * Must be sent before any video data.
 */
int rsp_rtmp_send_video_header(rsp_rtmp_t *ctx, int codec, const uint8_t *sps, uint32_t sps_len,
			       const uint8_t *pps, uint32_t pps_len, const uint8_t *vps,
			       uint32_t vps_len);

/*
 * Send a video frame (AVCC format, no parameter sets).
 * timestamp_ms is relative to stream start.
 */
int rsp_rtmp_send_video(rsp_rtmp_t *ctx, const uint8_t *avcc, uint32_t len, uint32_t timestamp_ms,
			bool is_key, int codec);

/*
 * Send AAC audio sequence header (AudioSpecificConfig).
 * Must be sent before any audio data.
 */
int rsp_rtmp_send_audio_header(rsp_rtmp_t *ctx, uint32_t sample_rate, uint8_t channels);

/*
 * Send an AAC audio frame (raw AU, no ADTS).
 * timestamp_ms is relative to stream start.
 */
int rsp_rtmp_send_audio(rsp_rtmp_t *ctx, const uint8_t *data, uint32_t len, uint32_t timestamp_ms);

/*
 * Disconnect and clean up.
 */
void rsp_rtmp_disconnect(rsp_rtmp_t *ctx);

#endif /* RSP_RTMP_H */

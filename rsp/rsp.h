/*
 * rsp.h -- RSP (Raptor Stream Push) internal state
 */

#ifndef RSP_H
#define RSP_H

#include <rss_ipc.h>
#include <rss_common.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

#include "rsp_rtmp.h"
#include "rsp_audio.h"
#include "rmr_nal.h"

typedef struct {
	/* Config */
	rss_config_t *cfg;
	const char *config_path;
	char url[512];
	int stream_idx;
	bool audio_enabled;
	int reconnect_delay;
	int tcp_sndbuf;

	/* Rings */
	rss_ring_t *video_ring;
	const char *video_ring_name;
	rss_ring_t *audio_ring;
	uint64_t video_read_seq;
	uint64_t audio_read_seq;

	/* Ring metadata */
	bool use_zerocopy;
	uint32_t video_codec;
	uint32_t width, height;
	uint32_t fps_num;
	uint32_t audio_codec;
	uint32_t audio_sample_rate;

	/* Codec params */
	rmr_codec_params_t params;

	/* Frame buffers (main thread only) */
	uint8_t *frame_buf;
	uint32_t frame_buf_size;
	uint8_t *avcc_buf;
	uint32_t avcc_buf_size;
	uint8_t audio_buf[8192];

	/* Audio transcoder (non-AAC → AAC) */
	rsp_audio_enc_t *audio_enc;

	/* Passthrough audio timestamp smoother */
	uint32_t audio_next_ts;
	bool audio_ts_running;

	/* RTMP connection */
	rsp_rtmp_t rtmp;
	bool header_sent;

	/* Parsed URL components */
	char host[256];
	int port;
	char app[256];
	char stream_key[512];
	bool use_tls;

	/* Control */
	rss_ctrl_t *ctrl;
	volatile sig_atomic_t *running;
	_Atomic bool pushing;

	/* Stats (main thread only) */
	uint64_t frames_sent;
	uint64_t frames_dropped;
	uint64_t bytes_sent;
	uint64_t audio_frames_sent;
	int64_t connect_time_us;
} rsp_state_t;

#endif /* RSP_H */

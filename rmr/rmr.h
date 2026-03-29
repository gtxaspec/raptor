/*
 * rmr.h -- RMR (Raptor Media Recorder) internal state
 */

#ifndef RMR_H
#define RMR_H

#include <rss_ipc.h>
#include <rss_common.h>

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

#include "rmr_mux.h"
#include "rmr_nal.h"
#include "rmr_prebuf.h"
#include "rmr_storage.h"

/* Recording mode */
#define RMR_MODE_CONTINUOUS 0 /* always recording */
#define RMR_MODE_MOTION	    1 /* record only on motion trigger */
#define RMR_MODE_BOTH	    2 /* continuous + motion clips */

typedef struct {
	/* Config */
	rss_config_t *cfg;
	const char *config_path;
	int stream_idx;
	bool audio_enabled;
	int mode; /* RMR_MODE_* */

	/* Rings */
	rss_ring_t *video_ring;
	rss_ring_t *audio_ring;
	uint64_t video_read_seq;
	uint64_t audio_read_seq;

	/* Ring metadata */
	uint32_t video_codec;
	uint32_t width, height;
	uint32_t fps_num;
	uint32_t audio_codec;
	uint32_t audio_sample_rate;

	/* Codec params */
	rmr_codec_params_t params;

	/* Muxer — continuous segment (main thread only) */
	rmr_mux_t *mux;
	rmr_storage_t *storage;
	int segment_fd;
	char segment_path[256];
	int64_t segment_start_us;

	/* Pre-buffer for motion clips (main thread only) */
	rmr_prebuf_t *video_pb;
	rmr_prebuf_t *audio_pb;
	int prebuffer_sec;
	int clip_length_sec;

	/* Motion clip context (main thread only) */
	rmr_storage_t *clip_storage;
	rmr_mux_t *clip_mux;
	int clip_fd;
	char clip_path[256];
	int64_t clip_v_ts_base; /* first video timestamp in clip */
	int64_t clip_a_dts;	/* audio DTS counter for clip */
	int64_t clip_start_us;	/* wall clock when clip opened */
	uint64_t clip_bytes;

	/* Frame read buffer (main thread only) */
	uint8_t *frame_buf;
	uint32_t frame_buf_size;

	/* AVCC conversion buffer (main thread only) */
	uint8_t *avcc_buf;
	uint32_t avcc_buf_size;

	/* Audio read buffer (main thread only) */
	uint8_t audio_buf[8192];

	/* Control */
	rss_ctrl_t *ctrl;

	/* State */
	volatile sig_atomic_t *running;
	_Atomic bool recording;	     /* continuous recording active */
	_Atomic bool clip_recording; /* motion clip active */

	/* Stats (main thread only) */
	uint64_t frames_written;
	uint64_t frames_dropped;
	uint64_t bytes_written;
	int frames_since_flush;

	/* Timestamps (main thread only) */
	int64_t v_dts;
	int64_t a_dts;
} rmr_state_t;

#endif /* RMR_H */

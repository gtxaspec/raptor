/*
 * rmr.h -- RMR (Raptor Media Recorder) internal state
 */

#ifndef RMR_H
#define RMR_H

#include <rss_ipc.h>
#include <rss_common.h>
#include <rss_sei.h>

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

#include "rmr_gps.h"
#include "rmr_io.h"
#include "rmr_mux.h"
#include "rmr_nal.h"
#include "rmr_prebuf.h"
#include "rmr_sign.h"
#include "rmr_storage.h"
#include "rmr_streams.h"
#include "rmr_timelapse.h"

/* Recording mode */
#define RMR_MODE_CONTINUOUS 0 /* always recording */
#define RMR_MODE_MOTION	    1 /* record only on motion trigger */
#define RMR_MODE_BOTH	    2 /* continuous + motion clips */

typedef struct rmr_state rmr_state_t;

/* One recorded video stream: its ring and every file it writes
 * (main thread only). */
typedef struct {
	rmr_state_t *st;
	char ring_name[RMR_RING_NAME_MAX];
	char tag[RMR_TAG_MAX]; /* file suffix; empty on the first track */

	/* Ring */
	rss_ring_t *ring;
	uint64_t read_seq;
	uint32_t incarnation;
	bool idle;	       /* attached, but no frame for RING_IDLE_US */
	int64_t idle_since_us; /* last frame read */
	int64_t idle_check_us;
	int64_t retry_us;
	bool attached_once;
	bool wait_key; /* drop until a keyframe after ring overflow */

	/* Ring metadata */
	uint32_t codec;
	uint32_t width, height;
	uint32_t fps_num;
	rmr_codec_params_t params;

	/* Frame read and AVCC conversion buffers */
	uint8_t *frame_buf;
	uint32_t frame_buf_size;
	uint8_t *avcc_buf;
	uint32_t avcc_buf_size;

	/* Clip pre-roll, sized from this stream's observed rate */
	rmr_prebuf_t *video_pb;
	int64_t rate_start_us;
	uint64_t rate_bytes;
	bool rate_sized;

	/* Continuous segment */
	rmr_mux_t *mux;
	int segment_fd;
	uint64_t segment_bytes; /* file offset for GPS records */
	char segment_path[256];
	int64_t segment_start_rt_us;	/* wall clock at open (boundary math) */
	int64_t segment_boundary_rt_us; /* next wall-clock split point */
	bool segment_idr_requested;	/* one sharpening IDR per boundary */
	uint32_t segment_stamp;		/* name stamp generation last used */
	rmr_sign_stream_t sign_seg;
	int64_t v_ts_base; /* first video timestamp in the segment */
	int64_t a_dts;	   /* audio DTS counter for the segment */
	int64_t a_ts_base; /* ring ts mapping base for a_dts steering */
	int frames_since_flush;

	/* Motion clip */
	rmr_mux_t *clip_mux;
	int clip_fd;
	char clip_path[256];
	uint32_t clip_stamp;
	rmr_sign_stream_t sign_clip;
	int64_t clip_v_ts_base;
	int64_t clip_a_dts;
	int64_t clip_a_ts_base;
	int64_t clip_start_us; /* monotonic, when the clip opened */
	uint64_t clip_bytes;

	/* Stats */
	uint64_t frames_written;
	uint64_t frames_dropped;
	uint64_t card_drops; /* of those, fragments the writer could not take */
	int64_t card_drop_log_us;
	uint64_t bytes_written;
	uint64_t frames_lost; /* source frames that never reached a file */
	uint32_t last_src_seq;
} rmr_track_t;

struct rmr_state {
	/* Config */
	rss_config_t *cfg;
	const char *config_path;
	bool audio_enabled;
	int mode;	   /* RMR_MODE_* */
	bool sei_timecode; /* embed ST 0604 UTC SEI per frame */
	bool gps;	   /* embed rtd telematics records */

	/* Provenance signing: one key, one chain per file */
	bool sign_enabled;
	rmr_sign_key_t sign_key;
	rmr_sign_stream_t sign_tl; /* timelapse file chain */

	/* Video tracks; the first one also feeds the timelapse */
	rmr_track_t tracks[RMR_MAX_TRACKS];
	int track_count;

	/* Audio, shared: written into every track's files */
	rss_ring_t *audio_ring;
	uint64_t audio_read_seq;
	uint32_t audio_codec;
	uint32_t audio_sample_rate;
	uint8_t audio_aot; /* AAC object type from ring header (2=LC, 5=HE v1) */
	uint32_t audio_bps;
	uint32_t audio_samples_per_frame; /* 0 = derive from length */
	uint64_t audio_last_ws;
	int64_t audio_idle_since_us;
	int64_t audio_retry_us;
	uint8_t audio_buf[8192];
	rmr_prebuf_t *audio_pb;

	/* Clips and pre-roll */
	int prebuffer_sec;
	uint32_t prebuffer_max_bytes; /* per track */
	int clip_length_sec;

	/* Storage, shared by every track: one quota, one cleanup */
	rmr_storage_t *storage;
	rmr_storage_t *clip_storage;
	rmr_name_stamp_t segment_name;
	rmr_name_stamp_t clip_name;

	/* Timelapse (main thread only) */
	bool tl_enabled; /* [timelapse] enabled, runtime-togglable */
	rmr_tl_t tl;
	rmr_storage_t *tl_storage;
	rmr_mux_t *tl_mux;
	int tl_fd;
	char tl_path[256];
	uint64_t tl_bytes;
	uint64_t tl_frames_total;
	int64_t tl_last_err_us; /* rate-limits open/write failure warns */

	/* Telematics (main thread only) */
	rss_ring_t *tele_ring;
	uint64_t tele_read_seq;
	int64_t tele_retry_us;
	rtd_record_t tele_hist[8]; /* recent records, for clip pre-roll */
	unsigned tele_hist_n;

	/* File I/O (writer thread) and crash safety */
	rmr_io_t *io;
	int sync_interval_ms; /* fdatasync open files this often; 0 = only at close */
	int64_t next_sync_us;

	/* Control */
	rss_ctrl_t *ctrl;

	/* State */
	volatile sig_atomic_t *running;
	_Atomic bool recording;	     /* continuous recording active */
	_Atomic bool clip_recording; /* motion clip active */
	_Atomic bool armed;	     /* recording switched on: config, enable, disable */
};

#endif /* RMR_H */

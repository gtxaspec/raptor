/*
 * rvd.h -- RVD internal shared state
 */

#ifndef RVD_H
#define RVD_H

#include <pthread.h>
#include <raptor_hal.h>
#include <rss_ipc.h>
#include <rss_common.h>
#include <stdatomic.h>
#include <stdio.h>

#include "rvd_ivs_raw.h"

#include "rvd_osd_pos.h"

#define RVD_MAX_SENSORS		3
/* Framesource slots per sensor (fs_chn = sensor * RVD_FS_PER_SENSOR + local).
 * Local 0 is main and 1 is sub; every slot after that can carry an optional
 * extra video stream ([streamN], N >= 2). */
#define RVD_FS_PER_SENSOR 3
#define RVD_MAX_EXTRA	  (RVD_MAX_SENSORS * (RVD_FS_PER_SENSOR - 2))
/* main+sub+jpeg0+jpeg1 per sensor, + the extra streams */
#define RVD_MAX_STREAMS		(RVD_MAX_SENSORS * 4 + RVD_MAX_EXTRA)
#define RVD_MAX_JPEG		(RVD_MAX_SENSORS * 2)
#define RVD_JPEG_STREAM_ID_BASE 0x20 /* JPEG ring stream_id namespace (video=idx, audio=0x10) */
#define RVD_OSD_MAX_REGIONS	16
#define RVD_OSD_RETRY_INTERVAL	50 /* check ticks (~5s at 10Hz) */
#define RVD_OSD_NAME_LEN	32

/*
 * Thread safety: enc_cfg/fs_cfg are written only by the ctrl handler
 * (main thread). The encoder thread reads width/height once at startup
 * before ctrl is active. Commands that modify width/height (set-resolution,
 * set-codec) stop the encoder thread first via do_stream_restart.
 */
typedef struct {
	rss_video_config_t enc_cfg;
	rss_fs_config_t fs_cfg;
	rss_ring_t *ring;
	int chn;	   /* encoder group/channel index */
	int fs_chn;	   /* framesource channel (sensor*RVD_FS_PER_SENSOR + local) */
	int sensor_idx;	   /* which sensor (0, 1, 2) */
	char cfg_sect[32]; /* config section name (e.g. "stream0", "sensor1_stream0") */
	bool enabled;
	bool nr_vbs_auto;	    /* nr_vbs came from the default, not the config */
	bool vbs_fallback_tried;    /* one-shot single-buffer fallback consumed */
	int64_t started_us;	    /* last rvd_stream_start, for the fallback grace */
	bool is_jpeg;		    /* true for snapshot channel */
	bool jpeg_idle;		    /* true = stop encoder when no consumers */
	bool on_demand;		    /* video: encoder runs only while the ring has readers */
	bool is_extra;		    /* [streamN], N >= 2: attached last (rvd_extra_attach) */
	int extra_slot;		    /* is_extra: index into extra_saved[] */
	bool no_osd;		    /* no OSD group: no overlays, privacy stops the encoder */
	char ring_type[16];	    /* video ring name before the sensor prefix */
	uint32_t linger_ms;	    /* on demand: keep encoding this long after the last reader */
	_Atomic uint32_t demand_start_ms; /* on demand: encoder last started (ms, wraps) */
	int64_t pulse_next_us;	    /* jpeg_pulse: earliest next RecvPic start */
	uintptr_t enc_buf_addrs[8]; /* refmode: unique virAddr bases seen */
	uint8_t enc_buf_count;	    /* refmode: number of unique buffers discovered */
	uint32_t active_fps_num;    /* sensor-rate override in effect (0 = follow
				     * enc_cfg). Transient: set-sensor-fps only,
				     * never persisted, so the configured rate in
				     * enc_cfg/config stays the camera's truth. */
	uint32_t active_fps_den;
} rvd_stream_t;

/* Per-OSD-region state (dynamic, name-based) */
typedef struct {
	char name[RVD_OSD_NAME_LEN];
	rss_osd_shm_t *shm;
	int hal_handle;
	uint32_t width;
	uint32_t height;
	uint8_t *local_buf;
	bool active;
	bool shown;
	/* Hidden because someone asked for it, as against not shown yet.
	 * Pushing content re-shows a region that has never been shown, which
	 * is how a region first appears; without this flag that same path
	 * undoes an explicit hide on the next frame the producer sends. */
	bool want_hidden;
	int no_update_ticks;
	int layer;
} rvd_osd_region_t;

/* Encoder thread argument (stable lifetime in rvd_state_t) */
typedef struct rvd_state rvd_state_t;
typedef struct {
	rvd_state_t *st;
	int idx;
} rvd_enc_thread_arg_t;

struct rvd_state {
	/* HAL */
	rss_hal_ctx_t *hal_ctx;
	const rss_hal_ops_t *ops;
	bool hal_initialized;
	bool v4l2_backend;		/* reporting only: which backend the config chose */
	const rss_hal_caps_t *hal_caps; /* the created backend's capability surface */

	/* Sensors */
	int sensor_count;

	/* Sensor frame rate at pipeline init — the restore target for the
	 * transient set-sensor-fps override (0/0 = backend can't report). */
	uint32_t sensor_base_fps_num;
	uint32_t sensor_base_fps_den;

	/* Streams */
	rvd_stream_t streams[RVD_MAX_STREAMS];
	int stream_count;

	/* Extra streams ([streamN], N >= 2), in config order. Each costs rmem
	 * (framesource buffers + encoder), so they are built after every base
	 * channel holds its buffers (rvd_extra_attach): one that doesn't fit is
	 * dropped, never main. Attached ones sit at the tail of streams[] from
	 * extra_base on; extra_saved[] keeps the config (with runtime changes)
	 * of each while detached. */
	rvd_stream_t extra_saved[RVD_MAX_EXTRA];
	int extra_count;
	int extra_base;

	/* Deferred full pipeline reinit (T20 save bayer: the old-SDK VBM
	 * pool does not survive a RAW flip in place; respond first, then
	 * the frame loop rebuilds everything) */
	_Atomic bool pending_pipeline_reinit;

	/* Low latency mode */
	bool low_latency;

	/* SPS VUI corrections (system section, parsed once at init so the
	 * encoder threads never touch the lazily-inserting config map) */
	bool vui_full_range;
	int vui_matrix; /* matrix_coefficients to declare, -1 = keep */

	/* Ring reference mode (zero-copy) */
	bool refmode;
	bool refmode_shm; /* true = SHM injection, false = /dev/rmem */
	uintptr_t rmem_virt_base;
	uint32_t rmem_size;
	uint32_t rmem_mmap_offset;

	/* Per-stream SHM for encoder output (SHM injection path) */
	void *enc_shm_addr[RVD_MAX_STREAMS];
	uint32_t enc_shm_size[RVD_MAX_STREAMS];
	int enc_shm_fd[RVD_MAX_STREAMS];

	/* Per-stream thread management */
	pthread_t enc_tids[RVD_MAX_STREAMS];
	rvd_enc_thread_arg_t enc_args[RVD_MAX_STREAMS];
	_Atomic bool stream_active[RVD_MAX_STREAMS]; /* per-stream run flag */

	/* OSD */
	bool osd_enabled;
	bool use_isp_osd;   /* true = ISP OSD (no bind chain), false = IPU OSD */
	int direct_sensors; /* ISP driver's direct (IVDC) sensor count, -1 = unreadable */
	rvd_osd_region_t osd_regions[RVD_MAX_STREAMS][RVD_OSD_MAX_REGIONS];
	int osd_region_count[RVD_MAX_STREAMS];
	int osd_retry_counter;
	uint32_t osd_pool_override; /* ROD-requested pool size in bytes, 0 = use default */
	pthread_mutex_t osd_lock;

	/* Privacy mode (full-frame cover, per-stream) */
	int privacy_handles[RVD_MAX_STREAMS]; /* HAL region handles, -1 if none */
	bool privacy[RVD_MAX_STREAMS];
	_Atomic bool pipeline_ready; /* set after FS enable + encoder start */

#define RVD_MAX_BIND_STAGES 5 /* FS [→ IVS] [→ OSD] → ENC + headroom */

	/* Pipeline bind chain (for clean unbind at deinit) */
	rss_cell_t bind_chain[RVD_MAX_STREAMS][RVD_MAX_BIND_STAGES];
	int bind_chain_len[RVD_MAX_STREAMS];

	/* Control */
	rss_ctrl_t *ctrl;

	/* Config */
	rss_config_t *cfg;
	const char *config_path;

	/* JPEG snapshots (one per video stream) */
	int jpeg_streams[RVD_MAX_JPEG]; /* stream indices, -1 if disabled */
	int jpeg_count;
	/* Each snapshot slot's sensor and its index among that sensor's slots,
	 * set for a skipped slot too: its ring is jpeg<local> behind the
	 * sensor's prefix (s1_jpeg0, s1_jpeg1), what consumers open. */
	int jpeg_sensor[RVD_MAX_JPEG];
	int jpeg_local[RVD_MAX_JPEG];

	volatile sig_atomic_t *running;

	/* IVS (motion / person detection) */
	pthread_t ivs_tid;
	bool ivs_enabled;
	_Atomic bool ivs_active;
	_Atomic bool ivs_motion;
	_Atomic int64_t ivs_motion_ts;
	void *ivs_algo_handle;
	int ivs_grp;
	int ivs_chn;
	int ivs_fs_chn;	    /* framesource channel IVS is bound to */
	bool ivs_persondet; /* true = persondet/jzdl algo */
	bool ivs_base_move; /* true = base_move algo (vs move) */
	bool ivs_jzdl;	    /* true = standalone JZDL inference */
	void *jzdl_handle;  /* opaque JZDL context */
	int jzdl_interval_ms; /* [motion] detect_interval_ms, 0 = every frame */
	int ivs_grid_x;	    /* move detection grid columns */
	int ivs_grid_y;	    /* move detection grid rows */
	int ivs_roi_count;  /* total ROI count for move detection */
	_Atomic int ivs_person_count;
	rss_ivs_detect_result_t ivs_detections; /* latest persondet results */
	int ivs_w, ivs_h; /* the frame detections are in; under ivs_det_lock */
	pthread_mutex_t ivs_det_lock;		/* protects ivs_detections */
	/* save raw on the JZDL thread's channel: that thread owns the channel's
	 * frame depth, so it copies one frame for the requester (rvd_ivs_grab_raw) */
	rvd_ivs_raw_t ivs_raw;
};

/* rvd_pipeline.c */
int rvd_pipeline_init(rvd_state_t *st);

/* A ring's shm name: "main", "sub", "jpeg<N>" for sensor 0, "s<i>_" + that
 * for the others. Every producer and lookup goes through this. */
static inline void rvd_ring_name(int sensor_idx, const char *type, char *buf, size_t len)
{
	if (sensor_idx == 0)
		rss_strlcpy(buf, type, len);
	else
		snprintf(buf, len, "s%d_%s", sensor_idx, type);
}

/* The ring name of snapshot slot j (0 .. jpeg_count - 1) */
static inline void rvd_jpeg_ring_name(const rvd_state_t *st, int j, char *buf, size_t len)
{
	char base[16];
	snprintf(base, sizeof(base), "jpeg%d", st->jpeg_local[j]);
	rvd_ring_name(st->jpeg_sensor[j], base, buf, len);
}
void rvd_pipeline_deinit(rvd_state_t *st);

/* Per-stream lifecycle (hot restart) */
int rvd_stream_init(rvd_state_t *st, int idx);
void rvd_stream_deinit(rvd_state_t *st, int idx);
int rvd_stream_restart_vbs_fallback(rvd_state_t *st, int chn);
int rvd_stream_start(rvd_state_t *st, int idx);
void rvd_apply_flip(rvd_state_t *st);
void rvd_stream_stop(rvd_state_t *st, int idx);

/* Extra streams: rvd_extra_attach builds every configured extra stream not
 * yet attached, in config order, skipping any that doesn't fit; it returns
 * how many are attached. rvd_extra_detach takes them all down. Detach before
 * rebuilding any base stream (rmem), attach again after. rvd_extra_index
 * gives the streams[] index of extra_saved[slot], or -1 when detached. */
int rvd_extra_attach(rvd_state_t *st);
void rvd_extra_detach(rvd_state_t *st);
int rvd_extra_index(const rvd_state_t *st, int slot);
/* streams[] index of the main stream of a sensor (-1 if none) */
int rvd_sensor_main(const rvd_state_t *st, int sensor_idx);

/* Republish geometry and rate into the ring header; rsd's SDP reads it. */
void rvd_stream_publish_info(rvd_state_t *st, int idx);

/* rvd_frame_loop.c */
void rvd_frame_loop(rvd_state_t *st, volatile sig_atomic_t *running);
void *rvd_encoder_thread(void *arg);

/* rvd_osd.c */
rvd_osd_region_t *rvd_osd_find_region(rvd_state_t *st, int stream, const char *name);
void rvd_osd_init_stream(rvd_state_t *st, int idx);
void rvd_osd_deinit_stream(rvd_state_t *st, int idx);
void rvd_osd_check(rvd_state_t *st);
void *rvd_osd_thread(void *arg);
void rvd_osd_set_privacy(rvd_state_t *st, bool enable, int stream);

/* rvd_ctrl.c */
int rvd_ctrl_handler(const char *cmd_json, char *resp_buf, int resp_buf_size, void *userdata);

/* rvd_ivs.c */
int rvd_ivs_init(rvd_state_t *st);
int rvd_ivs_start(rvd_state_t *st);
void rvd_ivs_stop(rvd_state_t *st);
void rvd_ivs_pause(rvd_state_t *st);
void rvd_ivs_resume(rvd_state_t *st);
void rvd_ivs_deinit(rvd_state_t *st);
void *rvd_ivs_thread(void *arg);
bool rvd_ivs_owns_fs(rvd_state_t *st, int fs_chn);
int rvd_ivs_grab_raw(rvd_state_t *st, uint8_t *buf, size_t cap, uint32_t *size, uint32_t *w,
		     uint32_t *h, int timeout_ms);

#endif /* RVD_H */

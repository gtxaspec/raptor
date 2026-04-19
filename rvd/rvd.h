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

#define RVD_MAX_SENSORS	       3
#define RVD_MAX_STREAMS	       (RVD_MAX_SENSORS * 4) /* main+sub+jpeg0+jpeg1 per sensor */
#define RVD_MAX_JPEG	       (RVD_MAX_SENSORS * 2)
#define RVD_OSD_REGIONS	       6
#define RVD_OSD_RETRY_INTERVAL 50 /* check ticks (~5s at 10Hz) */

/* OSD region roles (must match ROD naming) */
#define RVD_OSD_TIME	0
#define RVD_OSD_UPTIME	1
#define RVD_OSD_TEXT	2
#define RVD_OSD_LOGO	3
#define RVD_OSD_PRIVACY 4
#define RVD_OSD_DETECT	5

typedef struct {
	rss_video_config_t enc_cfg;
	rss_fs_config_t fs_cfg;
	rss_ring_t *ring;
	int chn;	   /* encoder group/channel index */
	int fs_chn;	   /* framesource channel (sensor*3 + local) */
	int sensor_idx;	   /* which sensor (0, 1, 2) */
	char cfg_sect[32]; /* config section name (e.g. "stream0", "sensor1_stream0") */
	bool enabled;
	bool is_jpeg;	/* true for snapshot channel */
	bool jpeg_idle; /* true = stop encoder when no consumers */
	uintptr_t enc_buf_base; /* refmode: first virAddr seen (encoder region start) */
} rvd_stream_t;

/* Per-OSD-region state */
typedef struct {
	rss_osd_shm_t *shm;
	int hal_handle; /* -1 if not created */
	uint32_t width;
	uint32_t height;
	uint8_t *local_buf; /* local copy for HAL (IMP DMA needs non-SHM memory) */
	bool active;
	bool shown;	     /* true after first ShowRgn(1) from update thread */
	int no_update_ticks; /* ticks since last dirty — detect dead producer */
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

	/* Sensors */
	int sensor_count;

	/* Streams */
	rvd_stream_t streams[RVD_MAX_STREAMS];
	int stream_count;

	/* Low latency mode */
	bool low_latency;

	/* Ring reference mode (zero-copy) */
	bool refmode;
	uintptr_t rmem_virt_base;
	uint32_t rmem_size;

	/* Per-stream thread management */
	pthread_t enc_tids[RVD_MAX_STREAMS];
	rvd_enc_thread_arg_t enc_args[RVD_MAX_STREAMS];
	_Atomic bool stream_active[RVD_MAX_STREAMS]; /* per-stream run flag */

	/* OSD */
	bool osd_enabled;
	bool use_isp_osd; /* true = ISP OSD (no bind chain), false = IPU OSD */
	rvd_osd_region_t osd_regions[RVD_MAX_STREAMS][RVD_OSD_REGIONS];
	int osd_retry_counter;
	pthread_mutex_t osd_lock; /* held during OSD region create/destroy */

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
	bool ivs_persondet; /* true = persondet/jzdl algo, false = move/base_move */
	bool ivs_jzdl;	    /* true = standalone JZDL inference */
	void *jzdl_handle;  /* opaque JZDL context */
	int ivs_grid_x;	    /* move detection grid columns */
	int ivs_grid_y;	    /* move detection grid rows */
	int ivs_roi_count;  /* total ROI count for move detection */
	_Atomic int ivs_person_count;
	rss_ivs_detect_result_t ivs_detections; /* latest persondet results */
	pthread_mutex_t ivs_det_lock;		/* protects ivs_detections */
};

/* rvd_pipeline.c */
int rvd_pipeline_init(rvd_state_t *st);
void rvd_pipeline_deinit(rvd_state_t *st);

/* Per-stream lifecycle (hot restart) */
int rvd_stream_init(rvd_state_t *st, int idx);
void rvd_stream_deinit(rvd_state_t *st, int idx);
int rvd_stream_start(rvd_state_t *st, int idx);
void rvd_stream_stop(rvd_state_t *st, int idx);

/* rvd_frame_loop.c */
void rvd_frame_loop(rvd_state_t *st, volatile sig_atomic_t *running);
void *rvd_encoder_thread(void *arg);

/* rvd_osd.c */
void rvd_osd_calc_position(int stream_w, int stream_h, int region_w, int region_h,
			   const char *pos_str, int *out_x, int *out_y);
void rvd_osd_init(rvd_state_t *st);
void rvd_osd_init_stream(rvd_state_t *st, int idx);
void rvd_osd_deinit_stream(rvd_state_t *st, int idx);
void rvd_osd_check(rvd_state_t *st);
void rvd_osd_deinit(rvd_state_t *st);
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

#endif /* RVD_H */

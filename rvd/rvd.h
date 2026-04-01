/*
 * rvd.h -- RVD internal shared state
 */

#ifndef RVD_H
#define RVD_H

#include <raptor_hal.h>
#include <rss_ipc.h>
#include <rss_common.h>

#define RVD_MAX_STREAMS	       4 /* main, sub, jpeg0, jpeg1 */
#define RVD_MAX_JPEG	       2
#define RVD_OSD_REGIONS	       5
#define RVD_OSD_RETRY_INTERVAL 50 /* check ticks (~5s at 10Hz) */

/* OSD region roles (must match ROD naming) */
#define RVD_OSD_TIME	0
#define RVD_OSD_UPTIME	1
#define RVD_OSD_TEXT	2
#define RVD_OSD_LOGO	3
#define RVD_OSD_PRIVACY 4

typedef struct {
	rss_video_config_t enc_cfg;
	rss_fs_config_t fs_cfg;
	rss_ring_t *ring;
	int chn; /* encoder channel index */
	bool enabled;
	bool is_jpeg; /* true for snapshot channel */
} rvd_stream_t;

/* Per-OSD-region state */
typedef struct {
	rss_osd_shm_t *shm;
	int hal_handle; /* -1 if not created */
	uint32_t width;
	uint32_t height;
	uint8_t *local_buf; /* local copy for HAL (IMP DMA needs non-SHM memory) */
	bool active;
	bool shown;          /* true after first ShowRgn(1) from update thread */
	int no_update_ticks; /* ticks since last dirty — detect dead producer */
} rvd_osd_region_t;

typedef struct {
	/* HAL */
	rss_hal_ctx_t *hal_ctx;
	const rss_hal_ops_t *ops;

	/* Streams */
	rvd_stream_t streams[RVD_MAX_STREAMS];
	int stream_count;

	/* Low latency mode */
	bool low_latency;

	/* OSD */
	bool osd_enabled;
	rvd_osd_region_t osd_regions[RVD_MAX_STREAMS][RVD_OSD_REGIONS];
	int osd_retry_counter;

	/* Privacy mode (full-frame black cover) */
	int privacy_handles[RVD_MAX_STREAMS]; /* HAL region handles, -1 if none */
	bool privacy_active;
	volatile bool pipeline_ready; /* set after FS enable + encoder start */

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
	int jpeg_quality;

	volatile sig_atomic_t *running;

	/* IVS (motion detection) */
	bool ivs_enabled;
	_Atomic bool ivs_active;
	_Atomic bool ivs_motion;
	_Atomic int64_t ivs_motion_ts;
	void *ivs_algo_handle;
	int ivs_grp;
	int ivs_chn;
} rvd_state_t;

/* rvd_pipeline.c */
int rvd_pipeline_init(rvd_state_t *st);
void rvd_pipeline_deinit(rvd_state_t *st);

/* rvd_frame_loop.c */
void rvd_frame_loop(rvd_state_t *st, volatile sig_atomic_t *running);

/* rvd_osd.c */
void rvd_osd_init(rvd_state_t *st);
void rvd_osd_check(rvd_state_t *st);
void rvd_osd_deinit(rvd_state_t *st);
void *rvd_osd_thread(void *arg);
void rvd_osd_set_privacy(rvd_state_t *st, bool enable);

/* rvd_ctrl.c */
int rvd_ctrl_handler(const char *cmd_json, char *resp_buf, int resp_buf_size, void *userdata);

/* rvd_ivs.c */
int rvd_ivs_init(rvd_state_t *st);
int rvd_ivs_start(rvd_state_t *st);
void rvd_ivs_stop(rvd_state_t *st);
void rvd_ivs_deinit(rvd_state_t *st);
void *rvd_ivs_thread(void *arg);

#endif /* RVD_H */

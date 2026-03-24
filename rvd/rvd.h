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
#define RVD_OSD_REGIONS	       4
#define RVD_OSD_RETRY_INTERVAL 50 /* check ticks (~5s at 10Hz) */

/* OSD region roles (must match ROD naming) */
#define RVD_OSD_TIME   0
#define RVD_OSD_UPTIME 1
#define RVD_OSD_TEXT   2
#define RVD_OSD_LOGO   3

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
	bool active;
} rvd_osd_region_t;

typedef struct {
	/* HAL */
	rss_hal_ctx_t *hal_ctx;
	const rss_hal_ops_t *ops;

	/* Streams */
	rvd_stream_t streams[RVD_MAX_STREAMS];
	int stream_count;

	/* OSD */
	bool osd_enabled;
	rvd_osd_region_t osd_regions[RVD_MAX_STREAMS][RVD_OSD_REGIONS];
	int osd_retry_counter;

	/* Control */
	rss_ctrl_t *ctrl;

	/* Config */
	rss_config_t *cfg;
	const char *config_path;

	/* JPEG snapshots (one per video stream) */
	int jpeg_streams[RVD_MAX_JPEG]; /* stream indices, -1 if disabled */
	int jpeg_count;
	int jpeg_quality;
	char jpeg_paths[RVD_MAX_JPEG][64]; /* /tmp/snapshot-0.jpg, etc */

	volatile sig_atomic_t *running;
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

#endif /* RVD_H */

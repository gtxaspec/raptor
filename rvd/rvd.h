/*
 * rvd.h -- RVD internal shared state
 */

#ifndef RVD_H
#define RVD_H

#include <raptor_hal.h>
#include <rss_ipc.h>
#include <rss_common.h>

#define RVD_MAX_STREAMS      3   /* main, sub, jpeg */

typedef struct {
	rss_video_config_t  enc_cfg;
	rss_fs_config_t     fs_cfg;
	rss_ring_t         *ring;
	int                 chn;          /* encoder channel index */
	bool                enabled;
	bool                is_jpeg;      /* true for snapshot channel */
} rvd_stream_t;

typedef struct {
	/* HAL */
	rss_hal_ctx_t       *hal_ctx;
	const rss_hal_ops_t *ops;

	/* Streams */
	rvd_stream_t         streams[RVD_MAX_STREAMS];
	int                  stream_count;

	/* OSD */
	rss_osd_shm_t       *osd_shm[RVD_MAX_STREAMS];

	/* Control */
	rss_ctrl_t          *ctrl;

	/* Config */
	rss_config_t        *cfg;
	const char          *config_path;

	/* JPEG snapshot */
	int                  jpeg_stream;   /* stream index for JPEG, -1 if disabled */
	int                  jpeg_quality;
	char                 jpeg_path[64]; /* e.g. /tmp/snapshot.jpg */

	volatile sig_atomic_t *running;
} rvd_state_t;

/* rvd_pipeline.c */
int  rvd_pipeline_init(rvd_state_t *st);
void rvd_pipeline_deinit(rvd_state_t *st);

/* rvd_frame_loop.c */
void rvd_frame_loop(rvd_state_t *st, volatile sig_atomic_t *running);

/* rvd_osd.c */
void rvd_osd_init(rvd_state_t *st);
void rvd_osd_check(rvd_state_t *st);
void rvd_osd_deinit(rvd_state_t *st);

#endif /* RVD_H */

/*
 * rvd_ivs.c -- IVS / JZDL detection pipeline management
 *
 * Two modes of operation:
 *   - IVS pipeline: binds to sub-stream FrameSource via IVS group,
 *     runs move/persondet/base_move algorithms through the SDK.
 *   - JZDL standalone: reads NV12 frames directly from FrameSource
 *     channel 1 and runs JZDL model inference, bypassing IVS entirely.
 *
 * Both modes store detection results atomically for RMD to query
 * via the RVD control socket.
 *
 * Vendor SDK init order (from T31 sample-Encoder-video-IVS-move.c):
 *   1. CreateGroup(0)          -- before bind
 *   2. Bind FS(1) → IVS(0)    -- before FS enable
 *   3. FS StreamOn             -- enable framesource
 *   4. CreateMoveInterface     -- after FS streaming
 *   5. CreateChn(2, iface)     -- after FS streaming
 *   6. RegisterChn(0, 2)       -- after FS streaming
 *   7. StartRecvPic(2)         -- after FS streaming
 */

#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

#include "rvd.h"

#ifdef IVS_DETECT
/* FrameSource direct access — for JZDL standalone inference.
 * Struct layout from imp_common.h, declared here to avoid SDK header dep. */
typedef struct {
	int index;
	int pool_idx;
	uint32_t width;
	uint32_t height;
	uint32_t pixfmt;
	uint32_t size;
	uint32_t phyAddr;
	uint32_t virAddr;
	int64_t timeStamp;
	int rotate_osdflag;
	uint32_t priv[0];
} IMPFrameInfo;

extern int IMP_FrameSource_SetFrameDepth(int chnNum, int depth);
extern int IMP_FrameSource_GetFrame(int chnNum, IMPFrameInfo **frame);
extern int IMP_FrameSource_ReleaseFrame(int chnNum, IMPFrameInfo *frame);
#endif

#define IVS_GRP 0 /* IVS groups are separate from encoder groups */
#define IVS_CHN 2 /* vendor sample uses chn 2 (0,1 used by encoder) */

/*
 * Phase A: called before pipeline bind.
 * Only creates the IVS group so it can be bound in the pipeline chain.
 */
int rvd_ivs_init(rvd_state_t *st)
{
	/* Require sub-stream — IVS binds to FS channel 1 */
	if (st->stream_count < 2) {
		RSS_WARN("IVS requires sub-stream (stream1) — motion detection disabled");
		return RSS_ERR;
	}

	st->ivs_grp = IVS_GRP;
	st->ivs_chn = IVS_CHN;
	st->ivs_fs_chn = st->streams[1].fs_chn;
	pthread_mutex_init(&st->ivs_det_lock, NULL);

	int ret = RSS_HAL_CALL(st->ops, ivs_create_group, st->hal_ctx, IVS_GRP);
	if (ret != 0) {
		RSS_ERROR("IVS: create group failed: %d", ret);
		return ret;
	}

	atomic_store(&st->ivs_active, true);
	RSS_INFO("IVS: group %d created (bind pending)", IVS_GRP);
	return RSS_OK;
}

/*
 * Phase B: called after FS enable + encoder start.
 * Creates the algo interface, channel, registers, and starts receiving.
 */
int rvd_ivs_start(rvd_state_t *st)
{
	if (!atomic_load(&st->ivs_active))
		return RSS_ERR;

	rss_config_t *cfg = st->cfg;
	const char *algo = rss_config_get_str(cfg, "motion", "algorithm", "move");
	int sensitivity = rss_config_get_int(cfg, "motion", "sensitivity", 3);
	int skip = rss_config_get_int(cfg, "motion", "skip_frames", 5);

	int w = st->streams[1].enc_cfg.width;
	int h = st->streams[1].enc_cfg.height;

	void *algo_handle = NULL;
	st->ivs_persondet = false;

#ifdef IVS_DETECT
	if (strcmp(algo, "yolo") == 0) {
		/* JZDL standalone mode — skip IVS pipeline, use FrameSource directly */
		rss_ivs_jzdl_param_t jp = {0};
		rss_strlcpy(jp.model_path,
			    rss_config_get_str(cfg, "motion", "model",
					       "/usr/share/models/magik_model_yolov5.bin"),
			    sizeof(jp.model_path));
		jp.width = w;
		jp.height = h;
		jp.num_classes = rss_config_get_int(cfg, "motion", "num_classes", 4);
		jp.conf_threshold =
			rss_config_get_int(cfg, "motion", "conf_threshold", 40) / 100.0f;
		jp.nms_threshold = rss_config_get_int(cfg, "motion", "nms_threshold", 50) / 100.0f;

		st->jzdl_handle = hal_jzdl_create(&jp);
		if (st->jzdl_handle) {
			st->ivs_persondet = true;
			st->ivs_jzdl = true;
			RSS_INFO("IVS: JZDL standalone mode on %dx%d", w, h);
			return RSS_OK; /* skip IVS channel creation */
		}
		RSS_ERROR("IVS: JZDL create failed");
		return RSS_ERR;
	} else
#endif
		if (strcmp(algo, "persondet") == 0) {
		rss_ivs_persondet_param_t pp = {0};
		pp.skip_frame_count = skip;
		pp.width = w;
		pp.height = h;
		pp.sensitivity = sensitivity;
		pp.det_distance = rss_config_get_int(cfg, "motion", "det_distance", 2);
		pp.motion_trigger = rss_config_get_bool(cfg, "motion", "motion_trigger", false);
		if (st->ops->ivs_create_persondet_interface)
			algo_handle = st->ops->ivs_create_persondet_interface(st->hal_ctx, &pp);
		if (algo_handle)
			st->ivs_persondet = true;
	} else if (strcmp(algo, "base_move") == 0) {
		rss_ivs_base_move_param_t bp = {0};
		bp.skip_frame_count = skip;
		bp.sense = sensitivity > 3 ? 3 : sensitivity;
		bp.width = w;
		bp.height = h;
		if (st->ops->ivs_create_base_move_interface)
			algo_handle = st->ops->ivs_create_base_move_interface(st->hal_ctx, &bp);
	} else {
		rss_ivs_move_param_t mp = {0};
		mp.skip_frame_count = skip;
		mp.width = w;
		mp.height = h;

		int roi_count = rss_config_get_int(cfg, "motion", "roi_count", 0);
		if (roi_count > 0 && roi_count <= RSS_IVS_MAX_ROI) {
			/* Explicit ROI regions from config */
			mp.roi_count = roi_count;
			for (int i = 0; i < roi_count; i++) {
				char key[16];
				snprintf(key, sizeof(key), "roi%d", i);
				const char *val = rss_config_get_str(cfg, "motion", key, "");
				int x0 = 0, y0 = 0, x1 = w - 1, y1 = h - 1;
				if (sscanf(val, "%d,%d,%d,%d", &x0, &y0, &x1, &y1) < 4)
					RSS_WARN("IVS: roi%d incomplete, using defaults", i);
				mp.roi[i] = (rss_rect_t){x0, y0, x1, y1};
				mp.sense[i] = sensitivity > 4 ? 4 : sensitivity;
			}
		} else {
			/* Auto grid — default 4x4 */
			const char *grid_str = rss_config_get_str(cfg, "motion", "grid", "4x4");
			int gx = 4, gy = 4;
			if (sscanf(grid_str, "%dx%d", &gx, &gy) < 2)
				RSS_WARN("IVS: invalid grid '%s', using 4x4", grid_str);
			if (gx < 1)
				gx = 1;
			if (gy < 1)
				gy = 1;
			if (gx * gy > RSS_IVS_MAX_ROI) {
				gx = 4;
				gy = 4;
			}

			mp.roi_count = gx * gy;
			st->ivs_grid_x = gx;
			st->ivs_grid_y = gy;
			st->ivs_roi_count = mp.roi_count;
			int cw = w / gx;
			int ch = h / gy;
			for (int row = 0; row < gy; row++) {
				for (int col = 0; col < gx; col++) {
					int idx = row * gx + col;
					mp.roi[idx] = (rss_rect_t){
						col * cw,
						row * ch,
						(col + 1) * cw - 1,
						(row + 1) * ch - 1,
					};
					mp.sense[idx] = sensitivity > 4 ? 4 : sensitivity;
				}
			}
			RSS_DEBUG("IVS: %dx%d grid (%d zones, %dx%d each)", gx, gy, mp.roi_count,
				  cw, ch);
		}

		if (st->ops->ivs_create_move_interface)
			algo_handle = st->ops->ivs_create_move_interface(st->hal_ctx, &mp);
	}

	if (!algo_handle) {
		RSS_ERROR("IVS: failed to create %s interface", algo);
		return RSS_ERR;
	}

	st->ivs_algo_handle = algo_handle;

	int ret;

	ret = RSS_HAL_CALL(st->ops, ivs_create_channel, st->hal_ctx, IVS_CHN, algo_handle);
	if (ret != 0) {
		RSS_ERROR("IVS: create channel %d failed: %d", IVS_CHN, ret);
		goto err_iface;
	}

	ret = RSS_HAL_CALL(st->ops, ivs_register_channel, st->hal_ctx, IVS_GRP, IVS_CHN);
	if (ret != 0) {
		RSS_ERROR("IVS: register channel failed: %d", ret);
		goto err_chn;
	}

	ret = RSS_HAL_CALL(st->ops, ivs_start, st->hal_ctx, IVS_CHN);
	if (ret != 0) {
		RSS_ERROR("IVS: start failed: %d", ret);
		goto err_unreg;
	}

	atomic_store(&st->ivs_motion, false);
	atomic_store(&st->ivs_motion_ts, 0);
	atomic_store(&st->ivs_person_count, 0);

	RSS_DEBUG("IVS: %s started on %dx%d (chn=%d, sensitivity=%d, skip=%d)", algo, w, h, IVS_CHN,
		  sensitivity, skip);
	return RSS_OK;

err_unreg:
	RSS_HAL_CALL(st->ops, ivs_unregister_channel, st->hal_ctx, IVS_CHN);
err_chn:
	RSS_HAL_CALL(st->ops, ivs_destroy_channel, st->hal_ctx, IVS_CHN);
err_iface:
	if (st->ivs_jzdl)
		RSS_HAL_CALL(st->ops, ivs_destroy_jzdl_interface, st->hal_ctx, algo_handle);
	else if (st->ivs_persondet)
		RSS_HAL_CALL(st->ops, ivs_destroy_persondet_interface, st->hal_ctx, algo_handle);
	else if (strcmp(algo, "base_move") == 0)
		RSS_HAL_CALL(st->ops, ivs_destroy_base_move_interface, st->hal_ctx, algo_handle);
	else
		RSS_HAL_CALL(st->ops, ivs_destroy_move_interface, st->hal_ctx, algo_handle);
	st->ivs_algo_handle = NULL;
	atomic_store(&st->ivs_active, false);
	return ret;
}

/*
 * Phase 1 deinit: stop receiving, unregister channel, destroy channel + interface.
 * Called BEFORE FS disable and unbind (vendor SDK requires this order).
 */
void rvd_ivs_stop(rvd_state_t *st)
{
	if (!atomic_load(&st->ivs_active))
		return;

#ifdef IVS_DETECT
	/* JZDL standalone — no IVS channel to stop */
	if (st->ivs_jzdl) {
		if (st->jzdl_handle) {
			hal_jzdl_destroy(st->jzdl_handle);
			st->jzdl_handle = NULL;
		}
		RSS_INFO("IVS: JZDL stopped");
		return;
	}
#endif

	RSS_HAL_CALL(st->ops, ivs_stop, st->hal_ctx, st->ivs_chn);
	usleep(500000); /* SDK needs delay after StopRecvPic (vendor sample uses sleep(1)) */

	RSS_HAL_CALL(st->ops, ivs_unregister_channel, st->hal_ctx, st->ivs_chn);
	RSS_HAL_CALL(st->ops, ivs_destroy_channel, st->hal_ctx, st->ivs_chn);

	if (st->ivs_algo_handle) {
		const char *algo = rss_config_get_str(st->cfg, "motion", "algorithm", "move");
		if (st->ivs_persondet)
			RSS_HAL_CALL(st->ops, ivs_destroy_persondet_interface, st->hal_ctx,
				     st->ivs_algo_handle);
		else if (strcmp(algo, "base_move") == 0)
			RSS_HAL_CALL(st->ops, ivs_destroy_base_move_interface, st->hal_ctx,
				     st->ivs_algo_handle);
		else
			RSS_HAL_CALL(st->ops, ivs_destroy_move_interface, st->hal_ctx,
				     st->ivs_algo_handle);
		st->ivs_algo_handle = NULL;
	}

	RSS_INFO("IVS: stopped");
}

/*
 * Lightweight pause/resume for hot restart — just StopRecvPic/StartRecvPic.
 * The channel, algo interface, and group stay alive. Used when the bind
 * chain needs to be torn down and rebuilt (stream-restart, set-resolution).
 * Full stop (rvd_ivs_stop) destroys the channel — SDK can't recreate it.
 */
void rvd_ivs_pause(rvd_state_t *st)
{
	if (!atomic_load(&st->ivs_active))
		return;
	RSS_HAL_CALL(st->ops, ivs_stop, st->hal_ctx, st->ivs_chn);
	RSS_INFO("IVS: paused");
}

void rvd_ivs_resume(rvd_state_t *st)
{
	if (!atomic_load(&st->ivs_active))
		return;
	RSS_HAL_CALL(st->ops, ivs_start, st->hal_ctx, st->ivs_chn);
	RSS_INFO("IVS: resumed");
}

/*
 * Phase 2 deinit: destroy group.
 * Called AFTER unbind (vendor SDK: DestroyGroup can't be called until after UnBind).
 */
void rvd_ivs_deinit(rvd_state_t *st)
{
	if (!atomic_load(&st->ivs_active))
		return;

	RSS_HAL_CALL(st->ops, ivs_destroy_group, st->hal_ctx, st->ivs_grp);
	pthread_mutex_destroy(&st->ivs_det_lock);

	atomic_store(&st->ivs_active, false);
	RSS_INFO("IVS: deinitialized");
}

static void ivs_process_move_result(rvd_state_t *st, void *result)
{
	rss_ivs_move_result_t *mr = (rss_ivs_move_result_t *)result;
	int gx = st->ivs_grid_x;
	int gy = st->ivs_grid_y;
	int total = st->ivs_roi_count;

	/* No grid info — explicit ROIs or unconfigured; fall back to simple flag */
	if (gx <= 0 || gy <= 0 || total <= 0) {
		bool motion = false;
		for (int i = 0; i < RSS_IVS_MAX_ROI; i++) {
			if (mr->ret_roi[i]) {
				motion = true;
				break;
			}
		}
		bool prev = atomic_load(&st->ivs_motion);
		atomic_store(&st->ivs_motion, motion);
		if (motion)
			atomic_store(&st->ivs_motion_ts, rss_timestamp_us());
		if (motion && !prev)
			RSS_DEBUG("IVS: motion detected");
		else if (!motion && prev)
			RSS_DEBUG("IVS: motion stopped");
		return;
	}

	/* Find bounding box of active zones in the grid */
	int min_col = gx, max_col = -1, min_row = gy, max_row = -1;
	for (int i = 0; i < total && i < RSS_IVS_MAX_ROI; i++) {
		if (mr->ret_roi[i]) {
			int col = i % gx;
			int row = i / gx;
			if (col < min_col)
				min_col = col;
			if (col > max_col)
				max_col = col;
			if (row < min_row)
				min_row = row;
			if (row > max_row)
				max_row = row;
		}
	}

	bool motion = max_col >= 0;
	int64_t now = rss_timestamp_us();
	bool prev = atomic_load(&st->ivs_motion);

	if (motion) {
		atomic_store(&st->ivs_motion, true);
		atomic_store(&st->ivs_motion_ts, now);

		/* Convert grid cells to pixel coordinates */
		int w = st->streams[1].enc_cfg.width;
		int h = st->streams[1].enc_cfg.height;
		int cw = w / gx;
		int ch = h / gy;

		pthread_mutex_lock(&st->ivs_det_lock);
		st->ivs_detections.count = 1;
		st->ivs_detections.timestamp = now;
		st->ivs_detections.detections[0].box = (rss_rect_t){
			min_col * cw,
			min_row * ch,
			(max_col + 1) * cw - 1,
			(max_row + 1) * ch - 1,
		};
		st->ivs_detections.detections[0].confidence = 1.0f;
		st->ivs_detections.detections[0].class_id = -1; /* motion, not classified */
		pthread_mutex_unlock(&st->ivs_det_lock);

		if (!prev)
			RSS_DEBUG("IVS: motion detected");
	} else {
		if (prev && (now - atomic_load(&st->ivs_motion_ts)) > 2000000) {
			atomic_store(&st->ivs_motion, false);
			pthread_mutex_lock(&st->ivs_det_lock);
			st->ivs_detections.count = 0;
			pthread_mutex_unlock(&st->ivs_det_lock);
			RSS_DEBUG("IVS: motion stopped");
		}
	}
}

static void ivs_process_persondet_result(rvd_state_t *st, void *result)
{
	/*
	 * persondet_param_output_t layout (from SDK header):
	 *   offset 0:  int count
	 *   offset 4:  int count_move
	 *   offset 8:  person_info[20]  — each 36 bytes:
	 *              IVSRect box (16 bytes: 4 ints)
	 *              IVSRect show_box (16 bytes: 4 ints)
	 *              float confidence (4 bytes)
	 *   offset 728: int64_t timeStamp
	 *
	 * All reads use memcpy to avoid strict-aliasing violations on the
	 * opaque vendor buffer (LTO TBAA could reorder direct casts).
	 */
#define PD_PERSON_OFF  8
#define PD_PERSON_SIZE 36
#define PD_SHOWBOX_OFF 16
#define PD_CONF_OFF    32
#define PD_MAX_PERSONS 20

	int count;
	memcpy(&count, result, sizeof(int));
	if (count < 0)
		count = 0;
	if (count > PD_MAX_PERSONS)
		count = PD_MAX_PERSONS;
	if (count > RSS_IVS_MAX_DETECTIONS)
		count = RSS_IVS_MAX_DETECTIONS;

	int64_t now = rss_timestamp_us();
	bool prev = atomic_load(&st->ivs_motion);

	if (count > 0) {
		atomic_store(&st->ivs_motion, true);
		atomic_store(&st->ivs_person_count, count);
		atomic_store(&st->ivs_motion_ts, now);

		pthread_mutex_lock(&st->ivs_det_lock);
		st->ivs_detections.count = count;
		st->ivs_detections.timestamp = now;

		const char *base = (const char *)result + PD_PERSON_OFF;
		for (int i = 0; i < count; i++) {
			const char *pi = base + i * PD_PERSON_SIZE;
			int sb[4];
			float conf;
			memcpy(sb, pi + PD_SHOWBOX_OFF, sizeof(sb));
			memcpy(&conf, pi + PD_CONF_OFF, sizeof(conf));

			st->ivs_detections.detections[i].box = (rss_rect_t){
				sb[0], sb[1],
				sb[2], sb[3],
			};
			st->ivs_detections.detections[i].confidence = conf;
			st->ivs_detections.detections[i].class_id = 0;
		}
		pthread_mutex_unlock(&st->ivs_det_lock);

#undef PD_PERSON_OFF
#undef PD_PERSON_SIZE
#undef PD_SHOWBOX_OFF
#undef PD_CONF_OFF
#undef PD_MAX_PERSONS

		if (!prev)
			RSS_DEBUG("IVS: %d person(s) detected", count);
	} else {
		/* No detection this frame — hold previous results for 2s
		 * so the OSD overlay doesn't flicker on brief gaps */
		int64_t last = atomic_load(&st->ivs_motion_ts);
		if (prev && (now - last) > 2000000) {
			atomic_store(&st->ivs_motion, false);
			atomic_store(&st->ivs_person_count, 0);
			pthread_mutex_lock(&st->ivs_det_lock);
			st->ivs_detections.count = 0;
			pthread_mutex_unlock(&st->ivs_det_lock);
			RSS_DEBUG("IVS: persons cleared");
		}
	}
}

#ifdef IVS_DETECT
/*
 * JZDL standalone inference thread — reads NV12 frames directly from
 * the IVS FrameSource channel and runs JZDL model inference.
 * Bypasses the IVS pipeline entirely.
 */
static void *rvd_jzdl_thread(void *arg)
{
	rvd_state_t *st = arg;
	int fs = st->ivs_fs_chn;

	RSS_INFO("JZDL inference thread started (fs_chn=%d)", fs);

	/* Enable frame depth so GetFrame works alongside encoder binding.
	 * Must be > 0 for GetFrame to return frames. */
	IMP_FrameSource_SetFrameDepth(fs, 1);

	while (*st->running && atomic_load(&st->ivs_active)) {
		IMPFrameInfo *frame = NULL;
		int ret = IMP_FrameSource_GetFrame(fs, &frame);
		if (ret != 0 || !frame) {
			usleep(100000);
			continue;
		}

		rss_ivs_detect_result_t result = {0};
		ret = hal_jzdl_detect(st->jzdl_handle, (const uint8_t *)frame->virAddr, &result);
		IMP_FrameSource_ReleaseFrame(fs, frame);

		if (ret != 0)
			continue;

		int64_t now = rss_timestamp_us();
		bool prev = atomic_load(&st->ivs_motion);

		if (result.count > 0) {
			atomic_store(&st->ivs_motion, true);
			atomic_store(&st->ivs_person_count, result.count);
			atomic_store(&st->ivs_motion_ts, now);

			pthread_mutex_lock(&st->ivs_det_lock);
			st->ivs_detections = result;
			st->ivs_detections.timestamp = now;
			pthread_mutex_unlock(&st->ivs_det_lock);

			if (!prev)
				RSS_DEBUG("JZDL: %d detection(s)", result.count);
		} else {
			int64_t last = atomic_load(&st->ivs_motion_ts);
			if (prev && (now - last) > 2000000) {
				atomic_store(&st->ivs_motion, false);
				atomic_store(&st->ivs_person_count, 0);
				pthread_mutex_lock(&st->ivs_det_lock);
				st->ivs_detections.count = 0;
				pthread_mutex_unlock(&st->ivs_det_lock);
				RSS_DEBUG("JZDL: detections cleared");
			}
		}
	}

	IMP_FrameSource_SetFrameDepth(fs, 0);
	RSS_INFO("JZDL inference thread exiting");
	return NULL;
}
#endif

void *rvd_ivs_thread(void *arg)
{
	rvd_state_t *st = arg;

#ifdef IVS_DETECT
	/* JZDL standalone mode — different thread function */
	if (st->ivs_jzdl)
		return rvd_jzdl_thread(arg);
#endif

	RSS_INFO("IVS poll thread started (algo=%s)", st->ivs_persondet ? "persondet" : "move");

	while (*st->running && atomic_load(&st->ivs_active)) {
		int ret = RSS_HAL_CALL(st->ops, ivs_poll_result, st->hal_ctx, st->ivs_chn, 1000);
		if (ret != 0)
			continue;

		void *result = NULL;
		ret = RSS_HAL_CALL(st->ops, ivs_get_result, st->hal_ctx, st->ivs_chn, &result);
		if (ret != 0 || !result)
			continue;

		if (st->ivs_persondet)
			ivs_process_persondet_result(st, result);
		else
			ivs_process_move_result(st, result);

		RSS_HAL_CALL(st->ops, ivs_release_result, st->hal_ctx, st->ivs_chn, result);
	}

	RSS_INFO("IVS poll thread exiting");
	return NULL;
}

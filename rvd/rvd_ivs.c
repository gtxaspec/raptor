/*
 * rvd_ivs.c -- IVS (motion detection) pipeline management
 *
 * Creates an IVS group bound to the sub-stream FrameSource, runs a
 * poll thread that stores motion results atomically for RMD to query
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
	pthread_mutex_init(&st->ivs_det_lock, NULL);

	int ret = RSS_HAL_CALL(st->ops, ivs_create_group, st->hal_ctx, IVS_GRP);
	if (ret != 0) {
		RSS_ERROR("IVS: create group failed: %d", ret);
		return ret;
	}

	st->ivs_active = true;
	RSS_INFO("IVS: group %d created (bind pending)", IVS_GRP);
	return RSS_OK;
}

/*
 * Phase B: called after FS enable + encoder start.
 * Creates the algo interface, channel, registers, and starts receiving.
 */
int rvd_ivs_start(rvd_state_t *st)
{
	if (!st->ivs_active)
		return RSS_ERR;

	rss_config_t *cfg = st->cfg;
	const char *algo = rss_config_get_str(cfg, "motion", "algorithm", "move");
	int sensitivity = rss_config_get_int(cfg, "motion", "sensitivity", 3);
	int skip = rss_config_get_int(cfg, "motion", "skip_frames", 5);

	int w = st->streams[1].enc_cfg.width;
	int h = st->streams[1].enc_cfg.height;

	void *algo_handle = NULL;
	st->ivs_persondet = false;

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
	} else if (strcmp(algo, "persondet") == 0) {
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
	st->ivs_active = false;
	return ret;
}

/*
 * Phase 1 deinit: stop receiving, unregister channel, destroy channel + interface.
 * Called BEFORE FS disable and unbind (vendor SDK requires this order).
 */
void rvd_ivs_stop(rvd_state_t *st)
{
	if (!st->ivs_active)
		return;

	/* JZDL standalone — no IVS channel to stop */
	if (st->ivs_jzdl) {
		if (st->jzdl_handle) {
			hal_jzdl_destroy(st->jzdl_handle);
			st->jzdl_handle = NULL;
		}
		RSS_INFO("IVS: JZDL stopped");
		return;
	}

	RSS_HAL_CALL(st->ops, ivs_stop, st->hal_ctx, st->ivs_chn);
	usleep(500000); /* SDK needs delay after StopRecvPic (vendor sample uses sleep(1)) */

	RSS_HAL_CALL(st->ops, ivs_unregister_channel, st->hal_ctx, st->ivs_chn);
	RSS_HAL_CALL(st->ops, ivs_destroy_channel, st->hal_ctx, st->ivs_chn);

	if (st->ivs_algo_handle) {
		const char *algo = rss_config_get_str(st->cfg, "motion", "algorithm", "move");
		if (st->ivs_jzdl)
			RSS_HAL_CALL(st->ops, ivs_destroy_jzdl_interface, st->hal_ctx,
				     st->ivs_algo_handle);
		else if (st->ivs_persondet)
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
 * Phase 2 deinit: destroy group.
 * Called AFTER unbind (vendor SDK: DestroyGroup can't be called until after UnBind).
 */
void rvd_ivs_deinit(rvd_state_t *st)
{
	if (!st->ivs_active)
		return;

	RSS_HAL_CALL(st->ops, ivs_destroy_group, st->hal_ctx, st->ivs_grp);
	pthread_mutex_destroy(&st->ivs_det_lock);

	st->ivs_active = false;
	RSS_INFO("IVS: deinitialized");
}

static void ivs_process_move_result(rvd_state_t *st, void *result)
{
	rss_ivs_move_result_t *mr = (rss_ivs_move_result_t *)result;
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

	if (motion && !prev) {
		char zones[128] = {0};
		int off = 0;
		for (int i = 0; i < RSS_IVS_MAX_ROI && off < (int)sizeof(zones) - 4; i++) {
			if (mr->ret_roi[i])
				off += snprintf(zones + off, sizeof(zones) - off, "%s%d",
						off > 0 ? "," : "", i);
		}
		RSS_DEBUG("IVS: motion detected (zones: %s)", zones);
	} else if (!motion && prev) {
		RSS_DEBUG("IVS: motion stopped");
	}
}

static void ivs_process_persondet_result(rvd_state_t *st, void *result)
{
	/*
	 * The SDK returns persondet_param_output_t* but we access it
	 * through the generic rss_ivs_detect_result_t layout.  The HAL
	 * result is opaque — we cast based on the known SDK struct layout:
	 *   { int count, int count_move, person_info[20], int64_t ts }
	 * where person_info = { IVSRect box, IVSRect show_box, float confidence }
	 *
	 * We read count + person[] fields at known offsets rather than
	 * including the vendor header (which isn't in the standard path).
	 */
	const int *raw = (const int *)result;
	int count = raw[0];
	if (count < 0)
		count = 0;
	if (count > RSS_IVS_MAX_DETECTIONS)
		count = RSS_IVS_MAX_DETECTIONS;

	int64_t now = rss_timestamp_us();
	bool prev = atomic_load(&st->ivs_motion);

	if (count > 0) {
		/* Active detection — update results and timestamp */
		atomic_store(&st->ivs_motion, true);
		atomic_store(&st->ivs_person_count, count);
		atomic_store(&st->ivs_motion_ts, now);

		pthread_mutex_lock(&st->ivs_det_lock);
		st->ivs_detections.count = count;
		st->ivs_detections.timestamp = now;

		/*
		 * persondet_param_output_t layout (from SDK header):
		 *   offset 0:  int count
		 *   offset 4:  int count_move
		 *   offset 8:  person_info[20]  — each 36 bytes:
		 *              IVSRect box (16 bytes: 4 ints)
		 *              IVSRect show_box (16 bytes: 4 ints)
		 *              float confidence (4 bytes)
		 *   offset 728: int64_t timeStamp
		 */
		const char *base = (const char *)result + 8;
		for (int i = 0; i < count; i++) {
			const char *pi = base + i * 36;
			const int *sb = (const int *)(pi + 16);
			float conf;
			memcpy(&conf, pi + 32, sizeof(float));

			st->ivs_detections.detections[i].box = (rss_rect_t){
				sb[0], sb[1], /* ul.x, ul.y */
				sb[2], sb[3], /* br.x, br.y */
			};
			st->ivs_detections.detections[i].confidence = conf;
			st->ivs_detections.detections[i].class_id = 0;
		}
		pthread_mutex_unlock(&st->ivs_det_lock);

		if (!prev)
			RSS_DEBUG("IVS: %d person(s) detected", count);
		for (int i = 0; i < count; i++) {
			rss_ivs_detection_t *d = &st->ivs_detections.detections[i];
			RSS_DEBUG("IVS: person[%d] box=(%d,%d)-(%d,%d) conf=%.2f", i, d->box.p0_x,
				  d->box.p0_y, d->box.p1_x, d->box.p1_y, (double)d->confidence);
		}
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

/*
 * JZDL standalone inference thread — reads NV12 frames directly from
 * FrameSource channel 1 (sub-stream) and runs JZDL model inference.
 * Bypasses the IVS pipeline entirely.
 */
static void *rvd_jzdl_thread(void *arg)
{
	rvd_state_t *st = arg;

	RSS_INFO("JZDL inference thread started");

	/* Enable frame depth so GetFrame works alongside encoder binding.
	 * Must be > 0 for GetFrame to return frames. */
	int depth_ret = IMP_FrameSource_SetFrameDepth(1, 1);
	RSS_INFO("JZDL: SetFrameDepth(1, 1) = %d", depth_ret);

	int frame_count = 0;
	while (*st->running) {
		IMPFrameInfo *frame = NULL;
		int ret = IMP_FrameSource_GetFrame(1, &frame);
		if (ret != 0 || !frame) {
			if (++frame_count % 50 == 1)
				RSS_DEBUG("JZDL: GetFrame failed (ret=%d, attempt=%d)", ret,
					  frame_count);
			usleep(100000);
			continue;
		}
		RSS_DEBUG("JZDL: got frame %ux%u virAddr=%p size=%u", frame->width, frame->height,
			  (void *)(uintptr_t)frame->virAddr, frame->size);
		frame_count = 0;

		rss_ivs_detect_result_t result = {0};
		ret = hal_jzdl_detect(st->jzdl_handle, (const uint8_t *)frame->virAddr, &result);
		IMP_FrameSource_ReleaseFrame(1, frame);

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
			for (int i = 0; i < result.count; i++) {
				rss_ivs_detection_t *d = &result.detections[i];
				RSS_DEBUG("JZDL: [%d] class=%d box=(%d,%d)-(%d,%d) conf=%.2f", i,
					  d->class_id, d->box.p0_x, d->box.p0_y, d->box.p1_x,
					  d->box.p1_y, (double)d->confidence);
			}
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

	IMP_FrameSource_SetFrameDepth(1, 0);
	RSS_INFO("JZDL inference thread exiting");
	return NULL;
}

void *rvd_ivs_thread(void *arg)
{
	rvd_state_t *st = arg;

	/* JZDL standalone mode — different thread function */
	if (st->ivs_jzdl)
		return rvd_jzdl_thread(arg);

	RSS_INFO("IVS poll thread started (algo=%s)", st->ivs_persondet ? "persondet" : "move");

	int poll_count = 0;
	while (*st->running) {
		int ret = RSS_HAL_CALL(st->ops, ivs_poll_result, st->hal_ctx, st->ivs_chn, 1000);
		if (ret != 0) {
			if (++poll_count % 10 == 0)
				RSS_DEBUG("IVS: poll timeout (%d cycles, ret=%d)", poll_count, ret);
			continue;
		}

		void *result = NULL;
		ret = RSS_HAL_CALL(st->ops, ivs_get_result, st->hal_ctx, st->ivs_chn, &result);
		if (ret != 0 || !result) {
			RSS_DEBUG("IVS: get_result failed (ret=%d, result=%p)", ret, result);
			continue;
		}
		poll_count = 0;

		if (st->ivs_persondet)
			ivs_process_persondet_result(st, result);
		else
			ivs_process_move_result(st, result);

		RSS_HAL_CALL(st->ops, ivs_release_result, st->hal_ctx, st->ivs_chn, result);
	}

	RSS_INFO("IVS poll thread exiting");
	return NULL;
}

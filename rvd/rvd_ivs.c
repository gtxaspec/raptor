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

	if (strcmp(algo, "base_move") == 0) {
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
				sscanf(val, "%d,%d,%d,%d", &x0, &y0, &x1, &y1);
				mp.roi[i] = (rss_rect_t){x0, y0, x1, y1};
				mp.sense[i] = sensitivity > 4 ? 4 : sensitivity;
			}
		} else {
			/* Auto grid — default 4x4 */
			const char *grid_str = rss_config_get_str(cfg, "motion", "grid", "4x4");
			int gx = 4, gy = 4;
			sscanf(grid_str, "%dx%d", &gx, &gy);
			if (gx < 1) gx = 1;
			if (gy < 1) gy = 1;
			if (gx * gy > RSS_IVS_MAX_ROI) { gx = 4; gy = 4; }

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
			RSS_INFO("IVS: %dx%d grid (%d zones, %dx%d each)", gx, gy, mp.roi_count, cw, ch);
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

	RSS_INFO("IVS: %s started on %dx%d (chn=%d, sensitivity=%d, skip=%d)",
		 algo, w, h, IVS_CHN, sensitivity, skip);
	return RSS_OK;

err_unreg:
	RSS_HAL_CALL(st->ops, ivs_unregister_channel, st->hal_ctx, IVS_CHN);
err_chn:
	RSS_HAL_CALL(st->ops, ivs_destroy_channel, st->hal_ctx, IVS_CHN);
err_iface:
	if (strcmp(algo, "base_move") == 0)
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

	RSS_HAL_CALL(st->ops, ivs_stop, st->hal_ctx, st->ivs_chn);
	usleep(500000); /* SDK needs delay after StopRecvPic (vendor sample uses sleep(1)) */

	RSS_HAL_CALL(st->ops, ivs_unregister_channel, st->hal_ctx, st->ivs_chn);
	RSS_HAL_CALL(st->ops, ivs_destroy_channel, st->hal_ctx, st->ivs_chn);

	if (st->ivs_algo_handle) {
		const char *algo = rss_config_get_str(st->cfg, "motion", "algorithm", "move");
		if (strcmp(algo, "base_move") == 0)
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

	st->ivs_active = false;
	RSS_INFO("IVS: deinitialized");
}

void *rvd_ivs_thread(void *arg)
{
	rvd_state_t *st = arg;

	RSS_INFO("IVS poll thread started");

	while (*st->running) {
		int ret = RSS_HAL_CALL(st->ops, ivs_poll_result, st->hal_ctx, st->ivs_chn, 1000);
		if (ret != 0)
			continue;

		void *result = NULL;
		ret = RSS_HAL_CALL(st->ops, ivs_get_result, st->hal_ctx, st->ivs_chn, &result);
		if (ret != 0 || !result)
			continue;

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
			/* Log which zones triggered */
			char zones[128] = {0};
			int off = 0;
			for (int i = 0; i < RSS_IVS_MAX_ROI && off < (int)sizeof(zones) - 4; i++) {
				if (mr->ret_roi[i])
					off += snprintf(zones + off, sizeof(zones) - off,
							"%s%d", off > 0 ? "," : "", i);
			}
			RSS_DEBUG("IVS: motion detected (zones: %s)", zones);
		} else if (!motion && prev) {
			RSS_DEBUG("IVS: motion stopped");
		}

		RSS_HAL_CALL(st->ops, ivs_release_result, st->hal_ctx, st->ivs_chn, result);
	}

	RSS_INFO("IVS poll thread exiting");
	return NULL;
}

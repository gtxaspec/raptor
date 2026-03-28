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

#include <stdatomic.h>
#include <string.h>

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
		mp.roi_count = 1;
		mp.roi[0] = (rss_rect_t){0, 0, w - 1, h - 1};
		mp.sense[0] = sensitivity > 4 ? 4 : sensitivity;
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

void rvd_ivs_deinit(rvd_state_t *st)
{
	if (!st->ivs_active)
		return;

	RSS_HAL_CALL(st->ops, ivs_stop, st->hal_ctx, st->ivs_chn);
	RSS_HAL_CALL(st->ops, ivs_unregister_channel, st->hal_ctx, st->ivs_chn);
	RSS_HAL_CALL(st->ops, ivs_destroy_channel, st->hal_ctx, st->ivs_chn);
	RSS_HAL_CALL(st->ops, ivs_destroy_group, st->hal_ctx, st->ivs_grp);

	/* Destroy algo interface */
	if (st->ivs_algo_handle) {
		const char *algo = rss_config_get_str(st->cfg, "motion", "algorithm", "move");
		if (strcmp(algo, "base_move") == 0)
			RSS_HAL_CALL(st->ops, ivs_destroy_base_move_interface, st->hal_ctx,
				     st->ivs_algo_handle);
		else
			RSS_HAL_CALL(st->ops, ivs_destroy_move_interface, st->hal_ctx,
				     st->ivs_algo_handle);
	}

	st->ivs_algo_handle = NULL;
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

		if (motion != prev)
			RSS_DEBUG("IVS: motion %s", motion ? "detected" : "stopped");

		RSS_HAL_CALL(st->ops, ivs_release_result, st->hal_ctx, st->ivs_chn, result);
	}

	RSS_INFO("IVS poll thread exiting");
	return NULL;
}

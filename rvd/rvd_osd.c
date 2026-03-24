/*
 * rvd_osd.c -- OSD SHM consumer + HAL region management
 *
 * Opens per-region OSD SHM double-buffers created by ROD, creates
 * HAL OSD regions with calculated screen positions, and pushes
 * updated bitmaps to hardware via the fast update_region_data path.
 *
 * Handles ROD starting after RVD via lazy SHM retry.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rvd.h"

static const char *region_names[] = {"time", "uptime", "text", "logo"};

#define OSD_MARGIN 10

static void calc_position(int stream_w, int stream_h, int region_w, int region_h, int role,
			  int *out_x, int *out_y)
{
	switch (role) {
	case RVD_OSD_TIME: /* top-left */
		*out_x = OSD_MARGIN;
		*out_y = OSD_MARGIN;
		break;
	case RVD_OSD_UPTIME: /* top-right */
		*out_x = stream_w - region_w - OSD_MARGIN;
		*out_y = OSD_MARGIN;
		break;
	case RVD_OSD_TEXT: /* bottom-left */
		*out_x = OSD_MARGIN;
		*out_y = stream_h - region_h - OSD_MARGIN;
		break;
	case RVD_OSD_LOGO: /* bottom-right */
		*out_x = stream_w - region_w - OSD_MARGIN;
		*out_y = stream_h - region_h - OSD_MARGIN;
		break;
	default:
		*out_x = OSD_MARGIN;
		*out_y = OSD_MARGIN;
		break;
	}

	/* Ensure non-negative and even (some SDKs require 2-aligned) */
	if (*out_x < 0)
		*out_x = 0;
	if (*out_y < 0)
		*out_y = 0;
	*out_x &= ~1;
	*out_y &= ~1;
}

/*
 * Try to open a region's SHM and create the HAL region.
 * Returns true if the region is now active.
 */
static bool try_open_region(rvd_state_t *st, int s, int r)
{
	rvd_osd_region_t *reg = &st->osd_regions[s][r];
	if (reg->active)
		return true;
	if (reg->shm)
		return false; /* SHM open but HAL creation failed — don't retry */

	char name[64];
	snprintf(name, sizeof(name), "osd_%d_%s", s, region_names[r]);

	reg->shm = rss_osd_open(name);
	if (!reg->shm)
		return false;

	/* Read dimensions from SHM */
	uint32_t w, h;
	const uint8_t *bitmap = rss_osd_get_active_buffer(reg->shm, &w, &h);
	if (!bitmap || w == 0 || h == 0) {
		rss_osd_close(reg->shm);
		reg->shm = NULL;
		return false;
	}

	reg->width = w;
	reg->height = h;

	/* Allocate local buffer — IMP SDK DMA requires non-SHM memory.
	 * We copy from SHM to this buffer before each HAL update. */
	uint32_t buf_size = w * h * 4;
	reg->local_buf = malloc(buf_size);
	if (!reg->local_buf) {
		rss_osd_close(reg->shm);
		reg->shm = NULL;
		return false;
	}
	memcpy(reg->local_buf, bitmap, buf_size);

	/* Calculate screen position */
	int stream_w = st->streams[s].enc_cfg.width;
	int stream_h = st->streams[s].enc_cfg.height;
	int x, y;
	calc_position(stream_w, stream_h, (int)w, (int)h, r, &x, &y);

	/* Create HAL region */
	rss_osd_region_t attr = {
		.type = RSS_OSD_PIC,
		.x = x,
		.y = y,
		.width = (int)w,
		.height = (int)h,
		.bitmap_data = reg->local_buf,
		.bitmap_fmt = RSS_PIXFMT_BGRA,
		.global_alpha_en = true,
		.fg_alpha = 255,
		.bg_alpha = 0,
		.layer = r,
	};

	int handle = -1;
	int ret = RSS_HAL_CALL(st->ops, osd_create_region, st->hal_ctx, &handle, &attr);
	if (ret != RSS_OK) {
		RSS_WARN("osd_create_region(%s) failed: %d", name, ret);
		return false;
	}

	int grp = st->streams[s].chn;
	ret = RSS_HAL_CALL(st->ops, osd_register_region, st->hal_ctx, handle, grp);
	if (ret != RSS_OK) {
		RSS_WARN("osd_register_region(%s, grp=%d) failed: %d", name, grp, ret);
		RSS_HAL_CALL(st->ops, osd_destroy_region, st->hal_ctx, handle);
		return false;
	}

	/* Skip osd_show_region — it calls IMP_OSD_Start which crashes the
	 * encoder when called after the pipeline is running. OSD groups are
	 * already started in pipeline init. Regions appear with default
	 * group attributes (show=0 by default, but SetRgnAttr has the data). */
	(void)grp;

	reg->hal_handle = handle;
	reg->active = true;

	RSS_INFO("osd region %s: %ux%u at (%d,%d) layer=%d handle=%d", name, w, h, x, y, r, handle);
	return true;
}

void rvd_osd_init(rvd_state_t *st)
{
	if (!st->osd_enabled)
		return;

	/* Init all region state */
	for (int s = 0; s < st->stream_count; s++) {
		for (int r = 0; r < RVD_OSD_REGIONS; r++) {
			st->osd_regions[s][r].shm = NULL;
			st->osd_regions[s][r].hal_handle = -1;
			st->osd_regions[s][r].active = false;
		}
	}

	/* Try to open regions (ROD may not be running yet) */
	int opened = 0;
	for (int s = 0; s < st->stream_count; s++) {
		if (st->streams[s].is_jpeg)
			continue;
		for (int r = 0; r < RVD_OSD_REGIONS; r++) {
			if (try_open_region(st, s, r))
				opened++;
		}
	}

	/* Start OSD groups (idempotent — safe to call again on lazy retry) */
	for (int s = 0; s < st->stream_count; s++) {
		if (st->streams[s].is_jpeg)
			continue;
		int grp = st->streams[s].chn;
		int ret = RSS_HAL_CALL(st->ops, osd_start, st->hal_ctx, grp);
		if (ret != RSS_OK)
			RSS_WARN("osd_start(%d) failed: %d", grp, ret);
	}

	st->osd_retry_counter = 0;
	RSS_INFO("osd init: %d regions active", opened);
}

void rvd_osd_check(rvd_state_t *st)
{
	if (!st->osd_enabled)
		return;

	/* Lazy retry: periodically try to open regions that aren't active yet */
	st->osd_retry_counter++;
	if (st->osd_retry_counter >= RVD_OSD_RETRY_INTERVAL) {
		st->osd_retry_counter = 0;
		for (int s = 0; s < st->stream_count; s++) {
			if (st->streams[s].is_jpeg)
				continue;
			for (int r = 0; r < RVD_OSD_REGIONS; r++) {
				if (!st->osd_regions[s][r].active)
					try_open_region(st, s, r);
			}
		}
	}

	/* Check dirty flags and push updates */
	for (int s = 0; s < st->stream_count; s++) {
		if (st->streams[s].is_jpeg)
			continue;
		for (int r = 0; r < RVD_OSD_REGIONS; r++) {
			rvd_osd_region_t *reg = &st->osd_regions[s][r];
			if (!reg->active || !reg->shm)
				continue;

			if (!rss_osd_check_dirty(reg->shm))
				continue;

			uint32_t w, h;
			const uint8_t *bitmap = rss_osd_get_active_buffer(reg->shm, &w, &h);
			if (!bitmap)
				continue;

			/* Copy SHM bitmap to local buffer, then update HAL.
			 * IMP SDK DMA reads from picData.pData — must not point
			 * into SHM mmap (causes DMA/cache coherency issues). */
			memcpy(reg->local_buf, bitmap, reg->width * reg->height * 4);
			RSS_HAL_CALL(st->ops, osd_update_region_data, st->hal_ctx, reg->hal_handle,
				     reg->local_buf);
			rss_osd_clear_dirty(reg->shm);
		}
	}
}

void *rvd_osd_thread(void *arg)
{
	rvd_state_t *st = arg;
	RSS_INFO("osd update thread started");

	while (*st->running) {
		rvd_osd_check(st);
		usleep(100000); /* 10 Hz */
	}

	RSS_INFO("osd update thread exiting");
	return NULL;
}

void rvd_osd_deinit(rvd_state_t *st)
{
	if (!st->osd_enabled)
		return;

	for (int s = 0; s < st->stream_count; s++) {
		if (st->streams[s].is_jpeg)
			continue;
		int grp = st->streams[s].chn;
		RSS_HAL_CALL(st->ops, osd_stop, st->hal_ctx, grp);

		for (int r = 0; r < RVD_OSD_REGIONS; r++) {
			rvd_osd_region_t *reg = &st->osd_regions[s][r];
			if (reg->active && reg->hal_handle >= 0) {
				RSS_HAL_CALL(st->ops, osd_show_region, st->hal_ctx, reg->hal_handle,
					     grp, 0);
				RSS_HAL_CALL(st->ops, osd_unregister_region, st->hal_ctx,
					     reg->hal_handle, grp);
				RSS_HAL_CALL(st->ops, osd_destroy_region, st->hal_ctx,
					     reg->hal_handle);
			}
			if (reg->shm) {
				rss_osd_close(reg->shm);
				reg->shm = NULL;
			}
			free(reg->local_buf);
			reg->local_buf = NULL;
			reg->active = false;
			reg->hal_handle = -1;
		}
	}
}

/*
 * rvd_osd.c -- OSD region management
 *
 * Creates OSD regions during pipeline init (before FS enable) with
 * transparent placeholder bitmaps. At runtime, polls OSD SHM for
 * bitmap updates from ROD and calls osd_update_region_data.
 *
 * IMP SDK constraint: CreateRgn/RegisterRgn must be called before the
 * encoder starts. Only UpdateRgnAttrData is safe at runtime.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rvd.h"

static const char *region_names[] = {"time", "uptime", "text", "logo"};

#define OSD_MARGIN 10

/* Region sizes must be >= ROD's rendered bitmap dimensions.
 * Over-allocating is fine — transparent padding is invisible. */
#define OSD_TEXT_W 1024
#define OSD_TEXT_H 48
#define OSD_LOGO_W 210
#define OSD_LOGO_H 64

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
	case RVD_OSD_TEXT: /* middle-left */
		*out_x = OSD_MARGIN;
		*out_y = (stream_h - region_h) / 2;
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
	if (*out_x < 0)
		*out_x = 0;
	if (*out_y < 0)
		*out_y = 0;
	*out_x &= ~1;
	*out_y &= ~1;
}

/*
 * Create a single OSD region with a transparent placeholder bitmap.
 * Called during pipeline init before FS enable.
 */
static bool create_region(rvd_state_t *st, int s, int r, uint32_t w, uint32_t h)
{
	rvd_osd_region_t *reg = &st->osd_regions[s][r];

	/* Ensure even dimensions */
	w = (w + 1) & ~1;
	h = (h + 1) & ~1;

	uint32_t buf_size = w * h * 4;
	reg->local_buf = calloc(1, buf_size); /* transparent (all zeros) */
	if (!reg->local_buf)
		return false;

	reg->width = w;
	reg->height = h;

	int stream_w = st->streams[s].enc_cfg.width;
	int stream_h = st->streams[s].enc_cfg.height;
	int x, y;
	calc_position(stream_w, stream_h, (int)w, (int)h, r, &x, &y);

	/* Per vendor SDK sample: pData = NULL at init, show = 0.
	 * Actual data set at runtime via UpdateRgnAttrData.
	 * ShowRgn called from update thread after first bitmap ready. */
	rss_osd_region_t attr = {
		.type = RSS_OSD_PIC,
		.x = x,
		.y = y,
		.width = (int)w,
		.height = (int)h,
		.bitmap_data = NULL,
		.bitmap_fmt = RSS_PIXFMT_BGRA,
		.global_alpha_en = true,
		.fg_alpha = 255,
		.bg_alpha = 0,
		.layer = r,
	};

	int handle = -1;
	int ret = RSS_HAL_CALL(st->ops, osd_create_region, st->hal_ctx, &handle, &attr);
	if (ret != RSS_OK) {
		RSS_WARN("osd_create_region(s%d/%s) failed: %d", s, region_names[r], ret);
		free(reg->local_buf);
		reg->local_buf = NULL;
		return false;
	}

	int grp = st->streams[s].chn;
	ret = RSS_HAL_CALL(st->ops, osd_register_region, st->hal_ctx, handle, grp);
	if (ret != RSS_OK) {
		RSS_WARN("osd_register_region(s%d/%s) failed: %d", s, region_names[r], ret);
		RSS_HAL_CALL(st->ops, osd_destroy_region, st->hal_ctx, handle);
		free(reg->local_buf);
		reg->local_buf = NULL;
		return false;
	}

	/* Init with show=0 (hidden). ShowRgn(1) called from update thread
	 * after first bitmap is ready, matching vendor SDK sample. */
	RSS_HAL_CALL(st->ops, osd_show_region, st->hal_ctx, handle, grp, 0);

	reg->hal_handle = handle;
	reg->active = true;

	RSS_INFO("osd region s%d/%s: %ux%u at (%d,%d) layer=%d handle=%d", s, region_names[r], w, h,
		 x, y, r, handle);
	return true;
}

/*
 * Create all OSD regions during pipeline init.
 * Must be called BEFORE osd_start and fs_enable.
 */
void rvd_osd_init(rvd_state_t *st)
{
	if (!st->osd_enabled)
		return;

	rss_config_t *cfg = st->cfg;

	for (int s = 0; s < st->stream_count; s++) {
		if (st->streams[s].is_jpeg)
			continue;

		for (int r = 0; r < RVD_OSD_REGIONS; r++) {
			st->osd_regions[s][r].shm = NULL;
			st->osd_regions[s][r].hal_handle = -1;
			st->osd_regions[s][r].active = false;
			st->osd_regions[s][r].local_buf = NULL;
		}

		/* Scale text dimensions for sub stream.
		 * Must be >= ROD's SHM sizes to avoid buffer overflows. */
		uint32_t tw = OSD_TEXT_W;
		uint32_t th = OSD_TEXT_H;
		if (s > 0) {
			int scale_num = st->streams[s].enc_cfg.height;
			int scale_den = st->streams[0].enc_cfg.height;
			if (scale_den == 0)
				scale_den = 1;
			tw = tw * scale_num / scale_den;
			th = th * scale_num / scale_den;
			if (tw < 480)
				tw = 480; /* min for sub stream */
			if (th < 20)
				th = 20;
		}

		int region_count = 0;

		if (rss_config_get_bool(cfg, "osd", "time_enabled", true)) {
			if (create_region(st, s, RVD_OSD_TIME, tw, th))
				region_count++;
		}

		if (rss_config_get_bool(cfg, "osd", "uptime_enabled", true)) {
			if (create_region(st, s, RVD_OSD_UPTIME, tw, th))
				region_count++;
		}

		if (rss_config_get_bool(cfg, "osd", "text_enabled", true)) {
			if (create_region(st, s, RVD_OSD_TEXT, tw, th))
				region_count++;
		}

		if (rss_config_get_bool(cfg, "osd", "logo_enabled", true)) {
			uint32_t lw = rss_config_get_int(cfg, "osd", "logo_width", OSD_LOGO_W);
			uint32_t lh = rss_config_get_int(cfg, "osd", "logo_height", OSD_LOGO_H);
			if (s > 0) {
				/* Sub stream logo — use smaller variant size */
				lw = 100;
				lh = 30;
			}
			if (create_region(st, s, RVD_OSD_LOGO, lw, lh))
				region_count++;
		}

		RSS_INFO("osd stream%d: %d regions created", s, region_count);
	}

	st->osd_retry_counter = 0;
}

/*
 * Try to open SHM for a region that doesn't have one yet.
 */
static void try_open_shm(rvd_state_t *st, int s, int r)
{
	rvd_osd_region_t *reg = &st->osd_regions[s][r];
	if (!reg->active || reg->shm)
		return;

	char name[64];
	snprintf(name, sizeof(name), "osd_%d_%s", s, region_names[r]);
	reg->shm = rss_osd_open(name);
	if (reg->shm)
		RSS_DEBUG("opened osd shm %s", name);
}

void rvd_osd_check(rvd_state_t *st)
{
	if (!st->osd_enabled)
		return;

	/* Lazy SHM open retry */
	st->osd_retry_counter++;
	if (st->osd_retry_counter >= RVD_OSD_RETRY_INTERVAL) {
		st->osd_retry_counter = 0;
		for (int s = 0; s < st->stream_count; s++) {
			if (st->streams[s].is_jpeg)
				continue;
			for (int r = 0; r < RVD_OSD_REGIONS; r++)
				try_open_shm(st, s, r);
		}
	}

	/* Push bitmap updates */
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

			if (w <= reg->width && h <= reg->height) {
				memset(reg->local_buf, 0, reg->width * reg->height * 4);
				for (uint32_t row = 0; row < h; row++)
					memcpy(reg->local_buf + row * reg->width * 4,
					       bitmap + row * w * 4, w * 4);
				RSS_HAL_CALL(st->ops, osd_update_region_data, st->hal_ctx,
					     reg->hal_handle, reg->local_buf);
			}
			rss_osd_clear_dirty(reg->shm);
		}
	}
}

void *rvd_osd_thread(void *arg)
{
	rvd_state_t *st = arg;
	RSS_INFO("osd update thread started");

	/* Wait for pipeline to be fully initialized before polling */
	sleep(2);

	/* Show all regions first (vendor pattern: ShowRgn before UpdateRgnAttrData) */
	for (int s = 0; s < st->stream_count; s++) {
		if (st->streams[s].is_jpeg)
			continue;
		int grp = st->streams[s].chn;
		for (int r = 0; r < RVD_OSD_REGIONS; r++) {
			rvd_osd_region_t *reg = &st->osd_regions[s][r];
			if (!reg->active)
				continue;
			RSS_HAL_CALL(st->ops, osd_show, st->hal_ctx, reg->hal_handle, grp, true);
			reg->shown = true;
		}
	}
	RSS_INFO("osd regions shown");

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

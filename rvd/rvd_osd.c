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

static const char *region_names[] = {"time", "uptime", "text", "logo", "privacy"};

#define OSD_MARGIN 10

/* Per-role region widths — sized to content, not max.
 * Must be >= ROD's SHM dimensions for that role. */
#define OSD_TIME_W   450 /* "2026-03-23 23:59:59" ≈ 420px at size 24 */
#define OSD_UPTIME_W 280 /* "12345d 23h 59m 59s" ≈ 260px */
#define OSD_TEXT_W   320 /* camera name */
#define OSD_TEXT_H   36
#define OSD_LOGO_W   210
#define OSD_LOGO_H   64

/* Default position names per role */
static const char *default_pos[] = {
	[RVD_OSD_TIME] = "top_left",   [RVD_OSD_UPTIME] = "top_right",
	[RVD_OSD_TEXT] = "top_center", [RVD_OSD_LOGO] = "bottom_right",
	[RVD_OSD_PRIVACY] = "center",
};

/*
 * Parse position string: named ("top_left", "bottom_right", etc.)
 * or coordinates ("100,50").
 */
static void calc_position(int stream_w, int stream_h, int region_w, int region_h,
			  const char *pos_str, int *out_x, int *out_y)
{
	/* Try x,y coordinates first */
	if (pos_str) {
		int x, y;
		if (sscanf(pos_str, "%d,%d", &x, &y) == 2) {
			*out_x = x;
			*out_y = y;
			goto clamp;
		}
	}

	/* Named positions */
	const char *p = pos_str ? pos_str : "top_left";

	if (strcmp(p, "top_left") == 0) {
		*out_x = OSD_MARGIN;
		*out_y = OSD_MARGIN;
	} else if (strcmp(p, "top_center") == 0) {
		*out_x = (stream_w - region_w) / 2;
		*out_y = OSD_MARGIN;
	} else if (strcmp(p, "top_right") == 0) {
		*out_x = stream_w - region_w - OSD_MARGIN;
		*out_y = OSD_MARGIN;
	} else if (strcmp(p, "bottom_left") == 0) {
		*out_x = OSD_MARGIN;
		*out_y = stream_h - region_h - OSD_MARGIN;
	} else if (strcmp(p, "bottom_center") == 0) {
		*out_x = (stream_w - region_w) / 2;
		*out_y = stream_h - region_h - OSD_MARGIN;
	} else if (strcmp(p, "bottom_right") == 0) {
		*out_x = stream_w - region_w - OSD_MARGIN;
		*out_y = stream_h - region_h - OSD_MARGIN;
	} else if (strcmp(p, "center") == 0) {
		*out_x = (stream_w - region_w) / 2;
		*out_y = (stream_h - region_h) / 2;
	} else {
		*out_x = OSD_MARGIN;
		*out_y = OSD_MARGIN;
	}

clamp:
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

	/* For logo, pre-load the BGRA file into local_buf so SetRgnAttr
	 * gets valid bitmap data (vendor pattern for static images). */
	if (r == RVD_OSD_LOGO) {
		const char *lp;
		int lw, lh;
		if (s == 0) {
			lp = rss_config_get_str(st->cfg, "osd", "logo_path",
						"/usr/share/images/thingino_100x30.bgra");
			lw = rss_config_get_int(st->cfg, "osd", "logo_width", 100);
			lh = rss_config_get_int(st->cfg, "osd", "logo_height", 30);
		} else {
			lp = "/usr/share/images/thingino_100x30.bgra";
			lw = 100;
			lh = 30;
		}
		int fsz;
		char *fd = rss_read_file(lp, &fsz);
		if (fd && fsz == lw * lh * 4) {
			for (int row = 0; row < lh && row < (int)h; row++)
				memcpy(reg->local_buf + row * w * 4, fd + row * lw * 4, lw * 4);
			RSS_INFO("logo %s (%dx%d) loaded into %ux%u region", lp, lw, lh, w, h);
		} else {
			RSS_WARN("logo load failed: %s (got %d bytes, expected %d)", lp, fsz,
				 lw * lh * 4);
		}
		free(fd);
	}

	int stream_w = st->streams[s].enc_cfg.width;
	int stream_h = st->streams[s].enc_cfg.height;

	/* Read per-stream position from config: stream0_time_pos, stream1_logo_pos, etc. */
	char pos_key[32];
	snprintf(pos_key, sizeof(pos_key), "stream%d_%s_pos", s, region_names[r]);
	const char *pos_str = rss_config_get_str(st->cfg, "osd", pos_key, default_pos[r]);

	int x, y;
	calc_position(stream_w, stream_h, (int)w, (int)h, pos_str, &x, &y);

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

	/* SetRgnAttr AFTER RegisterRgn (vendor SDK sample order) */
	ret = RSS_HAL_CALL(st->ops, osd_set_region_attr, st->hal_ctx, handle, &attr);
	if (ret != RSS_OK)
		RSS_WARN("osd_set_region_attr(s%d/%s) failed: %d", s, region_names[r], ret);

	/* Init with show=0. Layer r+1 (1-based, matching prudynt convention). */
	RSS_HAL_CALL(st->ops, osd_show_region, st->hal_ctx, handle, grp, 0, r + 1);

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

		/* Region sizes. For sub stream, use max of scaled size and
		 * ROD's SHM size (which is font-size dependent). */
		uint32_t th = OSD_TEXT_H;
		uint32_t time_w = OSD_TIME_W;
		uint32_t up_w = OSD_UPTIME_W;
		uint32_t txt_w = OSD_TEXT_W;

		if (s > 0) {
			int sw = st->streams[s].enc_cfg.width;
			int mw = st->streams[0].enc_cfg.width;
			int sh = st->streams[s].enc_cfg.height;
			int mh = st->streams[0].enc_cfg.height;
			if (mh > 0)
				th = th * sh / mh;
			if (th < 20)
				th = 20;
			/* Scale widths but clamp to min 240 (covers ROD sub output) */
			if (mw > 0) {
				time_w = time_w * sw / mw;
				up_w = up_w * sw / mw;
				txt_w = txt_w * sw / mw;
			}
			if (time_w < 240)
				time_w = 240;
			if (up_w < 150)
				up_w = 150;
			if (txt_w < 170)
				txt_w = 170;
		}

		int region_count = 0;

		if (rss_config_get_bool(cfg, "osd", "time_enabled", true)) {
			if (create_region(st, s, RVD_OSD_TIME, time_w, th))
				region_count++;
		}

		if (rss_config_get_bool(cfg, "osd", "uptime_enabled", true)) {
			if (create_region(st, s, RVD_OSD_UPTIME, up_w, th))
				region_count++;
		}

		if (rss_config_get_bool(cfg, "osd", "text_enabled", true)) {
			if (create_region(st, s, RVD_OSD_TEXT, txt_w, th))
				region_count++;
		}

		if (rss_config_get_bool(cfg, "osd", "logo_enabled", true)) {
			uint32_t lw = rss_config_get_int(cfg, "osd", "logo_width", OSD_LOGO_W);
			uint32_t lh = rss_config_get_int(cfg, "osd", "logo_height", OSD_LOGO_H);
			if (s > 0) {
				lw = 100;
				lh = 30;
			}
			if (create_region(st, s, RVD_OSD_LOGO, lw, lh))
				region_count++;
		}

		/* Privacy text region (centered, hidden until privacy mode) */
		if (create_region(st, s, RVD_OSD_PRIVACY, OSD_TEXT_W, th))
			region_count++;

		RSS_INFO("osd stream%d: %d regions created", s, region_count);

		/* Privacy cover region (full-frame black, initially hidden) */
		int stream_w = st->streams[s].enc_cfg.width;
		int stream_h = st->streams[s].enc_cfg.height;
		/* Privacy cover color from config (default black) */
		uint32_t pcol = (uint32_t)strtoul(
			rss_config_get_str(st->cfg, "osd", "privacy_color", "0xFF000000"), NULL, 0);
		rss_osd_region_t cover = {
			.type = RSS_OSD_COVER,
			.x = 0,
			.y = 0,
			.width = stream_w,
			.height = stream_h,
			.cover_color = pcol,
			.bitmap_fmt = RSS_PIXFMT_BGRA,
			.layer = 0, /* below text regions (1-4) so text shows on top */
		};
		int ph = -1;
		int pret = RSS_HAL_CALL(st->ops, osd_create_region, st->hal_ctx, &ph, &cover);
		if (pret == RSS_OK) {
			int grp = st->streams[s].chn;
			RSS_HAL_CALL(st->ops, osd_register_region, st->hal_ctx, ph, grp);
			RSS_HAL_CALL(st->ops, osd_set_region_attr, st->hal_ctx, ph, &cover);
			RSS_HAL_CALL(st->ops, osd_show_region, st->hal_ctx, ph, grp, 0, 0);
			st->privacy_handles[s] = ph;
			RSS_DEBUG("privacy cover region stream%d handle=%d", s, ph);
		} else {
			st->privacy_handles[s] = -1;
		}
	}

	st->privacy_active = false;
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
				if (w == reg->width && h == reg->height) {
					/* Exact match — flat copy */
					memcpy(reg->local_buf, bitmap, w * h * 4);
				} else {
					/* SHM smaller than region — row-by-row with padding */
					memset(reg->local_buf, 0, reg->width * reg->height * 4);
					for (uint32_t row = 0; row < h; row++)
						memcpy(reg->local_buf + row * reg->width * 4,
						       bitmap + row * w * 4, w * 4);
				}
				RSS_HAL_CALL(st->ops, osd_update_region_data, st->hal_ctx,
					     reg->hal_handle, reg->local_buf);
			}
			rss_osd_clear_dirty(reg->shm);
		}
	}
}

void rvd_osd_set_privacy(rvd_state_t *st, bool enable)
{
	if (!st->osd_enabled)
		return;

	for (int s = 0; s < st->stream_count; s++) {
		if (st->streams[s].is_jpeg)
			continue;
		int grp = st->streams[s].chn;

		/* Show/hide cover */
		int ph = st->privacy_handles[s];
		if (ph >= 0)
			RSS_HAL_CALL(st->ops, osd_show_region, st->hal_ctx, ph, grp, enable ? 1 : 0,
				     0);

		/* Show/hide privacy text region */
		rvd_osd_region_t *preg = &st->osd_regions[s][RVD_OSD_PRIVACY];
		if (preg->active && preg->hal_handle >= 0)
			RSS_HAL_CALL(st->ops, osd_show_region, st->hal_ctx, preg->hal_handle, grp,
				     enable ? 1 : 0, RVD_OSD_PRIVACY + 1);
	}

	st->privacy_active = enable;
	RSS_INFO("privacy mode %s", enable ? "ON" : "OFF");
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
			/* Privacy text starts hidden — shown only during privacy mode */
			if (r == RVD_OSD_PRIVACY)
				continue;
			RSS_HAL_CALL(st->ops, osd_show_region, st->hal_ctx, reg->hal_handle, grp, 1,
				     r + 1);
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
					     grp, 0, r + 1);
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

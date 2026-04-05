/*
 * rvd_osd.c -- OSD region management
 *
 * Creates OSD regions during pipeline init (before FS enable) with
 * transparent placeholder bitmaps. At runtime, polls OSD SHM for
 * bitmap updates from ROD and calls osd_update_region_data.
 *
 * WARNING: The IMP SDK OSD init sequence is order-sensitive and
 * underdocumented. The sequence below was derived from the vendor
 * sample (libimp-samples-musl/T31/libimp/sample-OSD.c) and verified
 * by trial-and-error. Deviations cause silent failures:
 *
 *   - CreateRgn with non-NULL attr → crash
 *   - SetRgnAttr before RegisterRgn → garbled bitmap
 *   - OSD_Start called from show_region → encoder deadlock
 *   - UpdateRgnAttrData with SHM mmap pointer → encoder hang
 *   - Regions created after encoder start → segfault
 *   - Multiple regions at same layer → garbled rendering
 *   - BGRA bitmaps > 100px wide → garbled on main stream
 *
 * Correct sequence (must not change without re-testing on device):
 *   CreateGroup → CreateRgn(NULL) → RegisterRgn → SetRgnAttr(pData=NULL)
 *   → SetGrpRgnAttr(show=0) → OSD_Start → System_Bind → FS_Enable
 *   → [runtime] UpdateRgnAttrData with heap buffer → ShowRgn from thread
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "rvd.h"

static const char *region_names[] = {"time", "uptime", "text", "logo", "privacy"};

#define OSD_MARGIN 10

/* Per-role region widths — sized to content, not max.
 * Must be >= ROD's SHM dimensions for that role. */
#define OSD_TIME_W   450 /* "2026-03-23 23:59:59" ≈ 420px at size 24 */
#define OSD_UPTIME_W 280 /* "12345d 23h 59m 59s" ≈ 260px */
#define OSD_TEXT_W   320 /* camera name */
#define OSD_TEXT_H   36
#define OSD_LOGO_W   100
#define OSD_LOGO_H   30

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

	/* Sanity check — prevent huge allocation from bad config */
	if (w > 4096 || h > 4096 || w == 0 || h == 0) {
		RSS_WARN("osd region s%d/%s: bad dimensions %ux%u", s, region_names[r], w, h);
		return false;
	}

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
		lp = rss_config_get_str(st->cfg, "osd", "logo", "");
		lw = rss_config_get_int(st->cfg, "osd", "logo_width", OSD_LOGO_W);
		lh = rss_config_get_int(st->cfg, "osd", "logo_height", OSD_LOGO_H);
		int fsz = 0;
		char *fd = rss_read_file(lp, &fsz);
		if (fd && fsz > 0 && fsz == lw * lh * 4) {
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
	int ret;

	if (st->use_isp_osd) {
		/* ISP OSD: create only — SetRgnAttr deferred to OSD thread
		 * (after FS enable, when ISP knows frame dimensions). */
		int sensor = st->streams[s].sensor_idx;
		ret = RSS_HAL_CALL(st->ops, isp_osd_create_region, st->hal_ctx, sensor, &handle);
		if (ret != RSS_OK) {
			RSS_WARN("isp_osd_create_region(s%d/%s) failed: %d", s, region_names[r], ret);
			free(reg->local_buf);
			reg->local_buf = NULL;
			return false;
		}
	} else {
		/* IPU OSD: create → register → set attr → hide */
		ret = RSS_HAL_CALL(st->ops, osd_create_region, st->hal_ctx, &handle, &attr);
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
	}

	reg->hal_handle = handle;
	reg->active = true;

	RSS_INFO("osd region s%d/%s: %ux%u at (%d,%d) layer=%d handle=%d%s", s, region_names[r], w,
		 h, x, y, r, handle, st->use_isp_osd ? " [isp]" : "");
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

	/* Initialize privacy handles for all streams (including skipped ones) */
	for (int s = 0; s < st->stream_count; s++) {
		st->privacy_handles[s] = -1;

	}

	for (int s = 0; s < st->stream_count; s++) {
		if (st->streams[s].is_jpeg)
			continue;
		/* ISP OSD: all regions render on all FS channels from that
		 * sensor — skip sub streams to avoid duplicate overlays. */
		if (st->use_isp_osd && s > 0 &&
		    st->streams[s].sensor_idx == st->streams[s - 1].sensor_idx)
			continue;

		/* Per-stream OSD enable/disable from config */
		{
			int si = st->streams[s].sensor_idx;
			int local_chn = st->streams[s].fs_chn % 3; /* 0=main, 1=sub */
			char sect[32];
			if (si == 0)
				snprintf(sect, sizeof(sect), "stream%d", local_chn);
			else
				snprintf(sect, sizeof(sect), "sensor%d_stream%d", si, local_chn);
			if (!rss_config_get_bool(cfg, sect, "osd_enabled", true)) {
				RSS_INFO("osd stream%d: disabled by [%s] osd_enabled", s, sect);
				continue;
			}
		}

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

		{
			const char *logo_path = rss_config_get_str(cfg, "osd", "logo", "");
			if (logo_path[0] && access(logo_path, R_OK) == 0) {
				uint32_t lw =
					rss_config_get_int(cfg, "osd", "logo_width", OSD_LOGO_W);
				uint32_t lh =
					rss_config_get_int(cfg, "osd", "logo_height", OSD_LOGO_H);
				if (create_region(st, s, RVD_OSD_LOGO, lw, lh))
					region_count++;
			}
		}

		/* Privacy text region (centered, hidden until privacy mode) */
		if (create_region(st, s, RVD_OSD_PRIVACY, OSD_TIME_W, th))
			region_count++;

		RSS_INFO("osd stream%d: %d regions created", s, region_count);

		/* Privacy cover region (full-frame, initially hidden).
		 * ISP OSD: allocated lazily in rvd_osd_set_privacy() to avoid
		 *   wasting ~8MB per stream at startup.
		 * IPU OSD: native cover type (just a color, no bitmap). */
		if (st->use_isp_osd) {
			st->privacy_handles[s] = -1;
	
		} else {
			int stream_w = st->streams[s].enc_cfg.width;
			int stream_h = st->streams[s].enc_cfg.height;
			uint32_t pcol = (uint32_t)strtoul(
				rss_config_get_str(st->cfg, "osd", "privacy_color", "0xFF000000"),
				NULL, 0);
			rss_osd_region_t cover = {
				.type = RSS_OSD_COVER,
				.x = 0,
				.y = 0,
				.width = stream_w,
				.height = stream_h,
				.cover_color = pcol,
				.bitmap_fmt = RSS_PIXFMT_BGRA,
				.layer = 0,
			};
			int ph = -1;
			int pret = RSS_HAL_CALL(st->ops, osd_create_region, st->hal_ctx, &ph,
						&cover);
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
	}

	st->privacy_active = false;
	st->osd_retry_counter = 0;
}

/*
 * Check if a named SHM object has been replaced (ROD restart).
 * Compares the inode of our open fd against the current named object.
 * Returns true if the SHM was replaced or removed.
 */
static bool shm_is_stale(rss_osd_shm_t *shm, const char *name)
{
	char path[128];
	snprintf(path, sizeof(path), "/dev/shm/rss_osd_%s", name);

	struct stat cur;
	if (stat(path, &cur) < 0)
		return true; /* SHM removed */

	struct stat ours;
	if (fstat(rss_osd_get_fd(shm), &ours) < 0)
		return true;

	return cur.st_ino != ours.st_ino;
}

/*
 * Push bitmap from local_buf to the hardware OSD region.
 * Handles both ISP OSD (SetRgnAttr with position) and IPU OSD (UpdateRgnAttrData).
 */
static void push_region(rvd_state_t *st, int s, int r)
{
	rvd_osd_region_t *reg = &st->osd_regions[s][r];
	if (!reg->active || !reg->local_buf)
		return;

	if (st->use_isp_osd) {
		int sensor = st->streams[s].sensor_idx;
		int stream_w = st->streams[s].enc_cfg.width;
		int stream_h = st->streams[s].enc_cfg.height;
		char pos_key[32];
		snprintf(pos_key, sizeof(pos_key), "stream%d_%s_pos", s, region_names[r]);
		const char *pos_str = rss_config_get_str(st->cfg, "osd", pos_key, default_pos[r]);
		int x, y;
		calc_position(stream_w, stream_h, (int)reg->width, (int)reg->height, pos_str, &x, &y);
		rss_osd_region_t attr = {
			.type = RSS_OSD_PIC,
			.x = x, .y = y,
			.width = (int)reg->width,
			.height = (int)reg->height,
			.bitmap_data = reg->local_buf,
			.bitmap_fmt = RSS_PIXFMT_BGRA,
		};
		int chx = st->streams[s].fs_chn % 3;
		RSS_HAL_CALL(st->ops, isp_osd_set_region_attr, st->hal_ctx, sensor,
			     reg->hal_handle, chx, &attr);
	} else {
		RSS_HAL_CALL(st->ops, osd_update_region_data, st->hal_ctx,
			     reg->hal_handle, reg->local_buf);
	}
}

/*
 * Copy SHM bitmap into local_buf (centering if smaller) and push to hardware.
 */
static void read_shm_and_push(rvd_state_t *st, int s, int r)
{
	rvd_osd_region_t *reg = &st->osd_regions[s][r];
	uint32_t w, h;
	const uint8_t *bitmap = rss_osd_get_active_buffer(reg->shm, &w, &h);
	if (!bitmap || w > reg->width || h > reg->height)
		return;

	if (w == reg->width && h == reg->height) {
		memcpy(reg->local_buf, bitmap, w * h * 4);
	} else {
		uint32_t x_off = (reg->width - w) / 2;
		memset(reg->local_buf, 0, reg->width * reg->height * 4);
		for (uint32_t row = 0; row < h; row++)
			memcpy(reg->local_buf + (row * reg->width + x_off) * 4,
			       bitmap + row * w * 4, w * 4);
	}
	push_region(st, s, r);
}

/*
 * Try to open SHM for a region that doesn't have one yet,
 * or reopen if the producer (ROD) restarted (different inode).
 */
static void try_open_shm(rvd_state_t *st, int s, int r)
{
	rvd_osd_region_t *reg = &st->osd_regions[s][r];
	if (!reg->active)
		return;

	if (reg->shm) {
		char name[64];
		snprintf(name, sizeof(name), "osd_%d_%s", s, region_names[r]);
		if (!shm_is_stale(reg->shm, name))
			return; /* same SHM, nothing to do */
		RSS_INFO("osd shm %s: producer restarted, reopening", name);
		rss_osd_close(reg->shm);
		reg->shm = NULL;
	}

	char name[64];
	snprintf(name, sizeof(name), "osd_%d_%s", s, region_names[r]);
	reg->shm = rss_osd_open(name);
	if (!reg->shm)
		return;
	RSS_DEBUG("opened osd shm %s", name);

	/* Read and push current buffer immediately — dirty flag may have
	 * been cleared by a previous RVD instance, but content is valid. */
	reg->no_update_ticks = 0;
	read_shm_and_push(st, s, r);
	rss_osd_clear_dirty(reg->shm);
}

void rvd_osd_check(rvd_state_t *st)
{
	if (!st->osd_enabled)
		return;

	/* Staleness check + lazy SHM open retry */
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

	/* Staleness check every ~1s: detect ROD death or restart.
	 * Two checks: (1) SHM inode changed (clean restart), (2) no dirty
	 * on ANY region for 3s (kill -9 / crash). Check 2 uses per-stream
	 * aggregate because static regions (logo, text) don't update
	 * continuously — only time/uptime do. */
	if ((st->osd_retry_counter % 10) != 0)
		goto push_updates;
	for (int s = 0; s < st->stream_count; s++) {
		if (st->streams[s].is_jpeg)
			continue;

		/* Check if any region on this stream got a dirty flag recently.
		 * If no regions have SHM open yet, assume alive — we can't
		 * declare ROD dead before we've ever connected. */
		bool any_alive = true;
		bool any_open = false;
		for (int r = 0; r < RVD_OSD_REGIONS; r++) {
			rvd_osd_region_t *reg = &st->osd_regions[s][r];
			if (reg->active && reg->shm) {
				any_open = true;
				if (reg->no_update_ticks < 30)
					break; /* at least one alive */
			}
		}
		if (any_open) {
			any_alive = false;
			for (int r = 0; r < RVD_OSD_REGIONS; r++) {
				rvd_osd_region_t *reg = &st->osd_regions[s][r];
				if (reg->active && reg->shm && reg->no_update_ticks < 30) {
					any_alive = true;
					break;
				}
			}
		}

		for (int r = 0; r < RVD_OSD_REGIONS; r++) {
			rvd_osd_region_t *reg = &st->osd_regions[s][r];
			if (!reg->active || !reg->shm)
				continue;

			bool stale = false;
			char name[64];
			snprintf(name, sizeof(name), "osd_%d_%s", s, region_names[r]);

			if (shm_is_stale(reg->shm, name))
				stale = true;
			else if (!any_alive)
				stale = true; /* all regions dead → ROD crashed */

			if (stale) {
				RSS_INFO("osd %s: producer gone, clearing", name);
				/* Remove orphaned SHM so we don't reopen it in a loop */
				if (!any_alive) {
					char path[128];
					snprintf(path, sizeof(path), "/dev/shm/rss_osd_%s", name);
					unlink(path);
				}
				rss_osd_close(reg->shm);
				reg->shm = NULL;
				reg->no_update_ticks = 0;
				if (reg->local_buf) {
					memset(reg->local_buf, 0,
					       reg->width * reg->height * 4);
					push_region(st, s, r);
				}
			}
		}

		/* Clean up orphaned SHM for regions RVD doesn't track
		 * (e.g. logo not configured in RVD but created by ROD) */
		if (!any_alive) {
			for (int r = 0; r < RVD_OSD_REGIONS; r++) {
				rvd_osd_region_t *reg = &st->osd_regions[s][r];
				if (reg->active && reg->shm)
					continue; /* handled above */
				char path[128];
				snprintf(path, sizeof(path), "/dev/shm/rss_osd_osd_%d_%s",
					 s, region_names[r]);
				unlink(path); /* no-op if doesn't exist */
			}
		}
	}

push_updates:
	/* Push bitmap updates */
	for (int s = 0; s < st->stream_count; s++) {
		if (st->streams[s].is_jpeg)
			continue;
		for (int r = 0; r < RVD_OSD_REGIONS; r++) {
			rvd_osd_region_t *reg = &st->osd_regions[s][r];
			if (!reg->active || !reg->shm)
				continue;

			if (!rss_osd_check_dirty(reg->shm)) {
				reg->no_update_ticks++;
				continue;
			}
			reg->no_update_ticks = 0;
			read_shm_and_push(st, s, r);
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
		if (st->use_isp_osd && s > 0 &&
		    st->streams[s].sensor_idx == st->streams[s - 1].sensor_idx)
			continue;

		/* Show/hide privacy cover */
		if (st->use_isp_osd) {
			/* ISP mask block: hardware color fill, no bitmap needed */
			int sensor = st->streams[s].sensor_idx;
			int chx = st->streams[s].fs_chn % 3;
			int stream_w = st->streams[s].enc_cfg.width;
			int stream_h = st->streams[s].enc_cfg.height;
			uint32_t pcol = (uint32_t)strtoul(
				rss_config_get_str(st->cfg, "osd", "privacy_color",
						   "0xFF000000"), NULL, 0);
			RSS_HAL_CALL(st->ops, isp_osd_set_mask, st->hal_ctx,
				     sensor, chx, 0, enable ? 1 : 0,
				     0, 0, stream_w, stream_h, pcol);
		} else {
			int ph = st->privacy_handles[s];
			if (ph >= 0) {
				int grp = st->streams[s].chn;
				RSS_HAL_CALL(st->ops, osd_show_region, st->hal_ctx, ph, grp,
					     enable ? 1 : 0, 0);
			}
		}

		/* Show/hide privacy text region */
		rvd_osd_region_t *preg = &st->osd_regions[s][RVD_OSD_PRIVACY];
		if (preg->active && preg->hal_handle >= 0) {
			if (st->use_isp_osd) {
				int sensor = st->streams[s].sensor_idx;
				RSS_HAL_CALL(st->ops, isp_osd_show_region, st->hal_ctx, sensor,
					     preg->hal_handle, enable ? 1 : 0);
			} else {
				int grp = st->streams[s].chn;
				RSS_HAL_CALL(st->ops, osd_show_region, st->hal_ctx,
					     preg->hal_handle, grp, enable ? 1 : 0,
					     RVD_OSD_PRIVACY + 1);
			}
		}
	}

	st->privacy_active = enable;
	RSS_INFO("privacy mode %s", enable ? "ON" : "OFF");
}

void *rvd_osd_thread(void *arg)
{
	rvd_state_t *st = arg;
	RSS_INFO("osd update thread started");

	/* Wait for pipeline to be fully initialized */
	while (!st->pipeline_ready && *st->running)
		usleep(100000);

	/* ISP OSD: now that FS is enabled, set initial region attrs + show.
	 * SetRgnAttr was deferred from init because the ISP needs frame
	 * dimensions which are only available after FS enable. */
	for (int s = 0; s < st->stream_count; s++) {
		if (st->streams[s].is_jpeg)
			continue;
		for (int r = 0; r < RVD_OSD_REGIONS; r++) {
			rvd_osd_region_t *reg = &st->osd_regions[s][r];
			if (!reg->active)
				continue;
			if (r == RVD_OSD_PRIVACY)
				continue;
			if (st->use_isp_osd) {
				int sensor = st->streams[s].sensor_idx;
				int stream_w = st->streams[s].enc_cfg.width;
				int stream_h = st->streams[s].enc_cfg.height;
				char pos_key[32];
				snprintf(pos_key, sizeof(pos_key), "stream%d_%s_pos",
					 s, region_names[r]);
				const char *pos_str = rss_config_get_str(
					st->cfg, "osd", pos_key, default_pos[r]);
				int x, y;
				calc_position(stream_w, stream_h,
					      (int)reg->width, (int)reg->height,
					      pos_str, &x, &y);
				rss_osd_region_t attr = {
					.type = RSS_OSD_PIC,
					.x = x, .y = y,
					.width = (int)reg->width,
					.height = (int)reg->height,
					.bitmap_data = reg->local_buf,
					.bitmap_fmt = RSS_PIXFMT_BGRA,
				};
				int chx = st->streams[s].fs_chn % 3;
				RSS_HAL_CALL(st->ops, isp_osd_set_region_attr, st->hal_ctx,
					     sensor, reg->hal_handle, chx, &attr);
				RSS_HAL_CALL(st->ops, isp_osd_show_region, st->hal_ctx, sensor,
					     reg->hal_handle, 1);
			} else {
				int grp = st->streams[s].chn;
				RSS_HAL_CALL(st->ops, osd_show_region, st->hal_ctx, reg->hal_handle,
					     grp, 1, r + 1);
			}
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
		if (st->use_isp_osd && s > 0 &&
		    st->streams[s].sensor_idx == st->streams[s - 1].sensor_idx)
			continue;

		if (!st->use_isp_osd) {
			int grp = st->streams[s].chn;
			RSS_HAL_CALL(st->ops, osd_stop, st->hal_ctx, grp);
		}

		for (int r = 0; r < RVD_OSD_REGIONS; r++) {
			rvd_osd_region_t *reg = &st->osd_regions[s][r];
			if (reg->active && reg->hal_handle >= 0) {
				if (st->use_isp_osd) {
					int sensor = st->streams[s].sensor_idx;
					RSS_HAL_CALL(st->ops, isp_osd_show_region, st->hal_ctx,
						     sensor, reg->hal_handle, 0);
					RSS_HAL_CALL(st->ops, isp_osd_destroy_region, st->hal_ctx,
						     sensor, reg->hal_handle);
				} else {
					int grp = st->streams[s].chn;
					RSS_HAL_CALL(st->ops, osd_show_region, st->hal_ctx,
						     reg->hal_handle, grp, 0, r + 1);
					RSS_HAL_CALL(st->ops, osd_unregister_region, st->hal_ctx,
						     reg->hal_handle, grp);
					RSS_HAL_CALL(st->ops, osd_destroy_region, st->hal_ctx,
						     reg->hal_handle);
				}
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

		/* Clean up privacy cover region */
		if (st->use_isp_osd && st->privacy_active) {
			/* Disable ISP mask block */
			int sensor = st->streams[s].sensor_idx;
			int chx = st->streams[s].fs_chn % 3;
			RSS_HAL_CALL(st->ops, isp_osd_set_mask, st->hal_ctx,
				     sensor, chx, 0, 0, 0, 0, 0, 0, 0);
		}
		int ph = st->privacy_handles[s];
		if (ph >= 0) {
			if (st->use_isp_osd) {
				/* No handle to destroy — mask blocks are stateless */
			} else {
				int grp = st->streams[s].chn;
				RSS_HAL_CALL(st->ops, osd_show_region, st->hal_ctx, ph, grp, 0, 0);
				RSS_HAL_CALL(st->ops, osd_unregister_region, st->hal_ctx, ph, grp);
				RSS_HAL_CALL(st->ops, osd_destroy_region, st->hal_ctx, ph);
			}
			st->privacy_handles[s] = -1;
		}
	}
}

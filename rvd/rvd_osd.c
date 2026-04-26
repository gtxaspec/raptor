/*
 * rvd_osd.c -- Dynamic OSD region management
 *
 * Discovers OSD SHM regions created by ROD by scanning /dev/shm for
 * files matching the naming convention osd_{stream}_{name}. Creates
 * and destroys HAL OSD regions dynamically as SHM objects appear and
 * disappear.
 *
 * WARNING: The IMP SDK OSD init sequence is order-sensitive and
 * underdocumented. The sequence below was derived from the vendor
 * sample (libimp-samples-musl/T31/libimp/sample-OSD.c) and verified
 * by trial-and-error. Deviations cause silent failures:
 *
 *   - CreateRgn with non-NULL attr -> crash
 *   - SetRgnAttr before RegisterRgn -> garbled bitmap
 *   - OSD_Start called from show_region -> encoder deadlock
 *   - UpdateRgnAttrData with SHM mmap pointer -> encoder hang
 *   - Regions created after encoder start -> segfault
 *   - Multiple regions at same layer -> garbled rendering
 *   - BGRA bitmaps > 100px wide -> garbled on main stream
 *
 * Correct sequence (must not change without re-testing on device):
 *   CreateGroup -> CreateRgn(NULL) -> RegisterRgn -> SetRgnAttr(pData=NULL)
 *   -> SetGrpRgnAttr(show=0) -> OSD_Start -> System_Bind -> FS_Enable
 *   -> [runtime] UpdateRgnAttrData with heap buffer -> ShowRgn from thread
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#include "rvd.h"

#define STREAM_ISP_OSD(st, s) ((st)->use_isp_osd && (st)->streams[s].fs_chn % 3 == 0)

#define OSD_MARGIN	      10

/*
 * Resolve position for an element. Lookup chain:
 *   1. stream{N}_{name}_pos in [osd] (set by ROD's osd-position IPC)
 *   2. position key in [osd.{name}] section (from config file)
 *   3. hardcoded fallback for well-known names
 */
static const char *resolve_position(rss_config_t *cfg, int s, const char *name)
{
	char pos_key[64];
	snprintf(pos_key, sizeof(pos_key), "stream%d_%s_pos", s, name);
	const char *pos = rss_config_get_str(cfg, "osd", pos_key, NULL);
	if (pos)
		return pos;

	char section[64];
	snprintf(section, sizeof(section), "osd.%s", name);
	pos = rss_config_get_str(cfg, section, "position", NULL);
	if (pos)
		return pos;

	return "top_left";
}

void rvd_osd_calc_position(int stream_w, int stream_h, int region_w, int region_h,
			   const char *pos_str, int *out_x, int *out_y)
{
	if (pos_str) {
		int x, y;
		if (sscanf(pos_str, "%d,%d", &x, &y) == 2) {
			*out_x = x;
			*out_y = y;
			goto clamp;
		}
	}

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
	} else if (strcmp(p, "left_center") == 0) {
		*out_x = OSD_MARGIN;
		*out_y = (stream_h - region_h) / 2;
	} else if (strcmp(p, "right_center") == 0) {
		*out_x = stream_w - region_w - OSD_MARGIN;
		*out_y = (stream_h - region_h) / 2;
	} else if (strcmp(p, "center") == 0) {
		*out_x = (stream_w - region_w) / 2;
		*out_y = (stream_h - region_h) / 2;
	} else {
		*out_x = OSD_MARGIN;
		*out_y = OSD_MARGIN;
	}

clamp:
	if (*out_x < OSD_MARGIN)
		*out_x = OSD_MARGIN;
	if (*out_y < OSD_MARGIN)
		*out_y = OSD_MARGIN;
	if (*out_x + region_w > stream_w - OSD_MARGIN)
		*out_x = stream_w - region_w - OSD_MARGIN;
	if (*out_y + region_h > stream_h - OSD_MARGIN)
		*out_y = stream_h - region_h - OSD_MARGIN;
	if (*out_x < OSD_MARGIN)
		*out_x = OSD_MARGIN;
	if (*out_y < OSD_MARGIN)
		*out_y = OSD_MARGIN;
	*out_x &= ~1;
	*out_y &= ~1;
}

/* ── Region lookup ── */

rvd_osd_region_t *rvd_osd_find_region(rvd_state_t *st, int stream, const char *name)
{
	if (stream < 0 || stream >= st->stream_count)
		return NULL;
	for (int r = 0; r < st->osd_region_count[stream]; r++) {
		if (st->osd_regions[stream][r].active &&
		    strcmp(st->osd_regions[stream][r].name, name) == 0)
			return &st->osd_regions[stream][r];
	}
	return NULL;
}

static rvd_osd_region_t *alloc_region_slot(rvd_state_t *st, int s)
{
	if (st->osd_region_count[s] < RVD_OSD_MAX_REGIONS) {
		rvd_osd_region_t *reg = &st->osd_regions[s][st->osd_region_count[s]];
		st->osd_region_count[s]++;
		return reg;
	}
	/* Reuse inactive slot */
	for (int r = 0; r < st->osd_region_count[s]; r++) {
		if (!st->osd_regions[s][r].active)
			return &st->osd_regions[s][r];
	}
	return NULL;
}

/* ── Region create/destroy ── */

static bool create_region(rvd_state_t *st, int s, rvd_osd_region_t *reg)
{
	uint32_t w = reg->width;
	uint32_t h = reg->height;

	w = (w + 1) & ~1u;
	h = (h + 1) & ~1u;

	if (w > 4096 || h > 4096 || w == 0 || h == 0) {
		RSS_WARN("osd region s%d/%s: bad dimensions %ux%u", s, reg->name, w, h);
		return false;
	}

	uint32_t buf_size = w * h * 4;
	reg->local_buf = calloc(1, buf_size);
	if (!reg->local_buf)
		return false;

	reg->width = w;
	reg->height = h;

	int stream_w = st->streams[s].enc_cfg.width;
	int stream_h = st->streams[s].enc_cfg.height;

	const char *pos_str = resolve_position(st->cfg, s, reg->name);
	int x, y;
	rvd_osd_calc_position(stream_w, stream_h, (int)w, (int)h, pos_str, &x, &y);

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
		.layer = reg->layer,
	};

	int handle = -1;
	int ret;
	bool is_isp = STREAM_ISP_OSD(st, s);

	if (is_isp) {
		int sensor = st->streams[s].sensor_idx;
		ret = RSS_HAL_CALL(st->ops, isp_osd_create_region, st->hal_ctx, sensor, &handle);
		if (ret != RSS_OK) {
			RSS_WARN("isp_osd_create_region(s%d/%s) failed: %d", s, reg->name, ret);
			free(reg->local_buf);
			reg->local_buf = NULL;
			return false;
		}
	} else {
		ret = RSS_HAL_CALL(st->ops, osd_create_region, st->hal_ctx, &handle, &attr);
		if (ret != RSS_OK) {
			RSS_WARN("osd_create_region(s%d/%s) failed: %d", s, reg->name, ret);
			free(reg->local_buf);
			reg->local_buf = NULL;
			return false;
		}

		int grp = st->streams[s].chn;
		ret = RSS_HAL_CALL(st->ops, osd_register_region, st->hal_ctx, handle, grp);
		if (ret != RSS_OK) {
			RSS_WARN("osd_register_region(s%d/%s) failed: %d", s, reg->name, ret);
			RSS_HAL_CALL(st->ops, osd_destroy_region, st->hal_ctx, handle);
			free(reg->local_buf);
			reg->local_buf = NULL;
			return false;
		}

		ret = RSS_HAL_CALL(st->ops, osd_set_region_attr, st->hal_ctx, handle, &attr);
		if (ret != RSS_OK)
			RSS_WARN("osd_set_region_attr(s%d/%s) failed: %d", s, reg->name, ret);

		RSS_HAL_CALL(st->ops, osd_show_region, st->hal_ctx, handle, grp, 0, reg->layer + 1);
	}

	reg->hal_handle = handle;
	reg->active = true;

	RSS_DEBUG("osd region s%d/%s: %ux%u at (%d,%d) layer=%d handle=%d%s", s, reg->name, w, h, x,
		  y, reg->layer, handle, is_isp ? " [isp]" : "");
	return true;
}

/*
 * Probe ROD SHM for a named region, get its dimensions.
 * Returns true if SHM exists and has valid dimensions.
 */
static bool probe_shm_dims(int s, const char *name, uint32_t *out_w, uint32_t *out_h)
{
	char shm_name[64];
	snprintf(shm_name, sizeof(shm_name), "osd_%d_%s", s, name);
	rss_osd_shm_t *probe = rss_osd_open(shm_name);
	if (!probe)
		return false;

	uint32_t w, h;
	rss_osd_get_active_buffer(probe, &w, &h);
	rss_osd_close(probe);
	if (w > 0 && h > 0) {
		*out_w = w;
		*out_h = h;
		return true;
	}
	return false;
}

/*
 * Create initial OSD regions by probing for known element SHMs.
 * Also scans /dev/shm for any ROD-created regions we don't know about.
 */
void rvd_osd_init_stream(rvd_state_t *st, int s)
{
	if (st->streams[s].is_jpeg)
		return;
	if (!rss_config_get_bool(st->cfg, st->streams[s].cfg_sect, "osd_enabled", true)) {
		RSS_INFO("osd stream%d: disabled by [%s] osd_enabled", s, st->streams[s].cfg_sect);
		return;
	}

	st->osd_region_count[s] = 0;
	for (int r = 0; r < RVD_OSD_MAX_REGIONS; r++) {
		st->osd_regions[s][r].shm = NULL;
		st->osd_regions[s][r].hal_handle = -1;
		st->osd_regions[s][r].active = false;
		st->osd_regions[s][r].local_buf = NULL;
		st->osd_regions[s][r].name[0] = '\0';
	}

	/* Scan /dev/shm for ROD-created SHM objects for this stream */
	char prefix[32];
	snprintf(prefix, sizeof(prefix), "rss_osd_osd_%d_", s);
	int prefix_len = (int)strlen(prefix);

	DIR *dir = opendir(RSS_SHM_DIR);
	if (!dir)
		return;

	int region_count = 0;
	int next_layer = 0;
	struct dirent *ent;
	while ((ent = readdir(dir)) != NULL) {
		if (strncmp(ent->d_name, prefix, prefix_len) != 0)
			continue;

		const char *name = ent->d_name + prefix_len;
		if (!name[0] || strlen(name) >= RVD_OSD_NAME_LEN)
			continue;

		/* Already have this region? */
		if (rvd_osd_find_region(st, s, name))
			continue;

		uint32_t w, h;
		if (!probe_shm_dims(s, name, &w, &h))
			continue;

		rvd_osd_region_t *reg = alloc_region_slot(st, s);
		if (!reg)
			break;

		memset(reg, 0, sizeof(*reg));
		rss_strlcpy(reg->name, name, sizeof(reg->name));
		reg->hal_handle = -1;
		reg->width = w;
		reg->height = h;
		reg->layer = next_layer++;

		/* T20 time+uptime merge: when both exist, make time full-width
		 * and mark uptime as virtual (composited into time region). */
		bool is_uptime_merged = false;
		if (strcmp(name, "uptime") == 0) {
			rvd_osd_region_t *time_reg = rvd_osd_find_region(st, s, "time");
			if (time_reg) {
				int stream_w = st->streams[s].enc_cfg.width;
				int merged_w = stream_w - 2 * OSD_MARGIN;
				if (merged_w < (int)time_reg->width)
					merged_w = (int)time_reg->width;
				time_reg->width = ((uint32_t)merged_w + 1) & ~1u;
				if (time_reg->local_buf) {
					uint8_t *nb = calloc(1, (size_t)time_reg->width *
									time_reg->height * 4);
					if (nb) {
						free(time_reg->local_buf);
						time_reg->local_buf = nb;
					}
				}
				reg->active = true;
				reg->hal_handle = -1;
				is_uptime_merged = true;
				region_count++;
			}
		} else if (strcmp(name, "time") == 0) {
			/* Check if uptime SHM also exists — pre-widen time */
			uint32_t uw, uh;
			if (probe_shm_dims(s, "uptime", &uw, &uh)) {
				int stream_w = st->streams[s].enc_cfg.width;
				int merged_w = stream_w - 2 * OSD_MARGIN;
				if (merged_w < (int)w)
					merged_w = (int)w;
				reg->width = ((uint32_t)merged_w + 1) & ~1u;
			}
		}

		if (!is_uptime_merged) {
			if (create_region(st, s, reg)) {
				region_count++;
			} else {
				reg->name[0] = '\0';
			}
		}
	}
	closedir(dir);

	RSS_DEBUG("osd stream%d: %d regions created", s, region_count);

	/* Privacy cover region (full-frame, initially hidden) */
	if (STREAM_ISP_OSD(st, s)) {
		st->privacy_handles[s] = -1;
	} else {
		int stream_w = st->streams[s].enc_cfg.width;
		int stream_h = st->streams[s].enc_cfg.height;
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
			.layer = 0,
		};
		int ph = -1;
		int pret = RSS_HAL_CALL(st->ops, osd_create_region, st->hal_ctx, &ph, &cover);
		if (pret == RSS_OK) {
			int grp = st->streams[s].chn;
			RSS_HAL_CALL(st->ops, osd_register_region, st->hal_ctx, ph, grp);
			RSS_HAL_CALL(st->ops, osd_set_region_attr, st->hal_ctx, ph, &cover);
			RSS_HAL_CALL(st->ops, osd_show_region, st->hal_ctx, ph, grp, 0, 0);
			st->privacy_handles[s] = ph;
		} else {
			st->privacy_handles[s] = -1;
		}
	}
}


/* ── SHM staleness and discovery ── */

static bool shm_is_stale(rss_osd_shm_t *shm, int s, const char *name)
{
	char path[128];
	snprintf(path, sizeof(path), RSS_SHM_DIR "/rss_osd_osd_%d_%s", s, name);

	struct stat cur;
	if (stat(path, &cur) < 0)
		return true;

	struct stat ours;
	if (fstat(rss_osd_get_fd(shm), &ours) < 0)
		return true;

	return cur.st_ino != ours.st_ino;
}

static void push_region(rvd_state_t *st, int s, rvd_osd_region_t *reg)
{
	if (!reg->active || !reg->local_buf)
		return;

	if (STREAM_ISP_OSD(st, s)) {
		int sensor = st->streams[s].sensor_idx;
		int stream_w = st->streams[s].enc_cfg.width;
		int stream_h = st->streams[s].enc_cfg.height;
		const char *pos_str = resolve_position(st->cfg, s, reg->name);
		int x, y;
		rvd_osd_calc_position(stream_w, stream_h, (int)reg->width, (int)reg->height,
				      pos_str, &x, &y);
		rss_osd_region_t attr = {
			.type = RSS_OSD_PIC,
			.x = x,
			.y = y,
			.width = (int)reg->width,
			.height = (int)reg->height,
			.bitmap_data = reg->local_buf,
			.bitmap_fmt = RSS_PIXFMT_BGRA,
		};
		int chx = st->streams[s].fs_chn % 3;
		RSS_HAL_CALL(st->ops, isp_osd_set_region_attr, st->hal_ctx, sensor, reg->hal_handle,
			     chx, &attr);
	} else {
		RSS_HAL_CALL(st->ops, osd_update_region_data, st->hal_ctx, reg->hal_handle,
			     reg->local_buf);
	}
}

static void read_shm_and_push(rvd_state_t *st, int s, rvd_osd_region_t *reg)
{
	if (!reg->local_buf)
		return;
	uint32_t w, h;
	const uint8_t *bitmap = rss_osd_get_active_buffer(reg->shm, &w, &h);
	if (!bitmap || w > reg->width || h > reg->height)
		return;

	if (w == reg->width && h == reg->height) {
		memcpy(reg->local_buf, bitmap, w * h * 4);
	} else {
		memset(reg->local_buf, 0, reg->width * reg->height * 4);
		for (uint32_t row = 0; row < h; row++)
			memcpy(reg->local_buf + row * reg->width * 4, bitmap + row * w * 4, w * 4);
	}
	push_region(st, s, reg);
}

/*
 * Try to open SHM for a region, or reopen if ROD restarted.
 * Handles SHM dimension changes by reallocating local_buf.
 */
static void try_open_shm(rvd_state_t *st, int s, rvd_osd_region_t *reg)
{
	if (!reg->active)
		return;

	char name[64];
	snprintf(name, sizeof(name), "osd_%d_%s", s, reg->name);

	if (reg->shm) {
		if (!shm_is_stale(reg->shm, s, reg->name))
			return;
		RSS_INFO("osd shm %s: producer restarted, reopening", name);
		rss_osd_close(reg->shm);
		reg->shm = NULL;
	}
	reg->shm = rss_osd_open(name);
	if (!reg->shm)
		return;
	RSS_DEBUG("opened osd shm %s", name);

	/* Handle SHM dimension changes (font size change) */
	if (reg->hal_handle >= 0) {
		/* Don't resize merged TIME region — intentionally wider */
		bool is_merged_time = false;
		if (strcmp(reg->name, "time") == 0) {
			rvd_osd_region_t *ureg = rvd_osd_find_region(st, s, "uptime");
			if (ureg && ureg->active && ureg->hal_handle < 0)
				is_merged_time = true;
		}

		uint32_t shm_w, shm_h;
		rss_osd_get_active_buffer(reg->shm, &shm_w, &shm_h);
		if (!is_merged_time && shm_w > 0 && shm_h > 0 &&
		    (shm_w != reg->width || shm_h != reg->height)) {
			uint32_t new_w = (shm_w + 1) & ~1u;
			uint32_t new_h = (shm_h + 1) & ~1u;
			uint32_t old_w = reg->width;
			uint32_t old_h = reg->height;
			uint8_t *new_buf = calloc(1, (size_t)new_w * new_h * 4);
			if (new_buf) {
				free(reg->local_buf);
				reg->local_buf = new_buf;
				reg->width = new_w;
				reg->height = new_h;

				int stream_w = st->streams[s].enc_cfg.width;
				int stream_h = st->streams[s].enc_cfg.height;
				const char *pos_str = resolve_position(st->cfg, s, reg->name);
				int x, y;
				rvd_osd_calc_position(stream_w, stream_h, (int)new_w, (int)new_h,
						      pos_str, &x, &y);

				rss_osd_region_t attr = {
					.type = RSS_OSD_PIC,
					.x = x,
					.y = y,
					.width = (int)new_w,
					.height = (int)new_h,
					.bitmap_data = reg->local_buf,
					.bitmap_fmt = RSS_PIXFMT_BGRA,
					.global_alpha_en = true,
					.fg_alpha = 255,
					.bg_alpha = 0,
					.layer = reg->layer,
				};
				if (STREAM_ISP_OSD(st, s)) {
					int sensor = st->streams[s].sensor_idx;
					int chx = st->streams[s].fs_chn % 3;
					RSS_HAL_CALL(st->ops, isp_osd_set_region_attr, st->hal_ctx,
						     sensor, reg->hal_handle, chx, &attr);
				} else {
					int grp = st->streams[s].chn;
					if (reg->shown)
						RSS_HAL_CALL(st->ops, osd_show_region, st->hal_ctx,
							     reg->hal_handle, grp, 0,
							     reg->layer + 1);
					RSS_HAL_CALL(st->ops, osd_set_region_attr, st->hal_ctx,
						     reg->hal_handle, &attr);
					if (reg->shown)
						RSS_HAL_CALL(st->ops, osd_show_region, st->hal_ctx,
							     reg->hal_handle, grp, 1,
							     reg->layer + 1);
				}
				RSS_DEBUG("osd %s: resized %ux%u -> %ux%u at (%d,%d)", name, old_w,
					  old_h, new_w, new_h, x, y);
			}
		}
	}

	if (reg->hal_handle < 0)
		return;

	reg->no_update_ticks = 0;
	read_shm_and_push(st, s, reg);
	rss_osd_clear_dirty(reg->shm);

	/* Privacy stays hidden until activated */
	if (!reg->shown && strcmp(reg->name, "privacy") == 0 && !st->privacy[s])
		return;

	if (!reg->shown) {
		if (STREAM_ISP_OSD(st, s)) {
			int sensor = st->streams[s].sensor_idx;
			int stream_w = st->streams[s].enc_cfg.width;
			int stream_h = st->streams[s].enc_cfg.height;
			const char *pos_str = resolve_position(st->cfg, s, reg->name);
			int x, y;
			rvd_osd_calc_position(stream_w, stream_h, (int)reg->width, (int)reg->height,
					      pos_str, &x, &y);
			rss_osd_region_t attr = {
				.type = RSS_OSD_PIC,
				.x = x,
				.y = y,
				.width = (int)reg->width,
				.height = (int)reg->height,
				.bitmap_data = reg->local_buf,
				.bitmap_fmt = RSS_PIXFMT_BGRA,
			};
			int chx = st->streams[s].fs_chn % 3;
			RSS_HAL_CALL(st->ops, isp_osd_set_region_attr, st->hal_ctx, sensor,
				     reg->hal_handle, chx, &attr);
			RSS_HAL_CALL(st->ops, isp_osd_show_region, st->hal_ctx, sensor,
				     reg->hal_handle, 1);
		} else {
			int grp = st->streams[s].chn;
			RSS_HAL_CALL(st->ops, osd_show_region, st->hal_ctx, reg->hal_handle, grp, 1,
				     reg->layer + 1);
		}
		reg->shown = true;
		RSS_DEBUG("osd %d/%s: shown on open", s, reg->name);
	}
}

/*
 * Scan for new SHM objects that appeared since last check.
 * Creates HAL regions for newly discovered elements.
 */
static void scan_new_shm(rvd_state_t *st, int s)
{
	char prefix[32];
	snprintf(prefix, sizeof(prefix), "rss_osd_osd_%d_", s);
	int prefix_len = (int)strlen(prefix);

	DIR *dir = opendir(RSS_SHM_DIR);
	if (!dir)
		return;

	struct dirent *ent;
	while ((ent = readdir(dir)) != NULL) {
		if (strncmp(ent->d_name, prefix, prefix_len) != 0)
			continue;

		const char *name = ent->d_name + prefix_len;
		if (!name[0] || strlen(name) >= RVD_OSD_NAME_LEN)
			continue;

		if (rvd_osd_find_region(st, s, name))
			continue;

		uint32_t w, h;
		if (!probe_shm_dims(s, name, &w, &h))
			continue;

		rvd_osd_region_t *reg = alloc_region_slot(st, s);
		if (!reg)
			break;

		memset(reg, 0, sizeof(*reg));
		rss_strlcpy(reg->name, name, sizeof(reg->name));
		reg->hal_handle = -1;
		reg->width = w;
		reg->height = h;
		reg->layer = st->osd_region_count[s];

		/* T20 time+uptime merge: avoid two OSD channels spanning
		 * opposite edges of the same scanline (causes IPU stall). */
		bool is_merged = false;
		if (strcmp(name, "uptime") == 0) {
			rvd_osd_region_t *treg = rvd_osd_find_region(st, s, "time");
			if (treg) {
				int sw = st->streams[s].enc_cfg.width;
				int mw = sw - 2 * OSD_MARGIN;
				if (mw > (int)treg->width) {
					treg->width = ((uint32_t)mw + 1) & ~1u;
					/* Reallocate time's local_buf for new width */
					uint8_t *nb =
						calloc(1, (size_t)treg->width * treg->height * 4);
					if (nb) {
						free(treg->local_buf);
						treg->local_buf = nb;
					}
				}
				reg->active = true;
				reg->hal_handle = -1;
				is_merged = true;
				RSS_INFO("osd stream%d: discovered '%s' (merged into time)", s,
					 name);
			}
		} else if (strcmp(name, "time") == 0) {
			uint32_t uw, uh;
			if (probe_shm_dims(s, "uptime", &uw, &uh)) {
				int sw = st->streams[s].enc_cfg.width;
				int mw = sw - 2 * OSD_MARGIN;
				if (mw > (int)w)
					reg->width = ((uint32_t)mw + 1) & ~1u;
			}
		}

		if (!is_merged) {
			if (create_region(st, s, reg)) {
				RSS_INFO("osd stream%d: discovered new element '%s' (%ux%u)", s,
					 name, w, h);
			} else {
				reg->name[0] = '\0';
			}
		}
	}
	closedir(dir);
}

/* ── Main OSD check (10 Hz) ── */

void rvd_osd_check(rvd_state_t *st)
{
	if (!st->osd_enabled)
		return;

	pthread_mutex_lock(&st->osd_lock);

	st->osd_retry_counter++;
	if (st->osd_retry_counter >= RVD_OSD_RETRY_INTERVAL) {
		st->osd_retry_counter = 0;
		for (int s = 0; s < st->stream_count; s++) {
			if (st->streams[s].is_jpeg)
				continue;
			for (int r = 0; r < st->osd_region_count[s]; r++)
				try_open_shm(st, s, &st->osd_regions[s][r]);
			scan_new_shm(st, s);
		}
	}

	/* Staleness check every ~1s */
	if ((st->osd_retry_counter % 10) != 0)
		goto push_updates;
	for (int s = 0; s < st->stream_count; s++) {
		if (st->streams[s].is_jpeg)
			continue;

		bool any_alive = true;
		bool any_open = false;
		for (int r = 0; r < st->osd_region_count[s]; r++) {
			rvd_osd_region_t *reg = &st->osd_regions[s][r];
			if (reg->active && reg->shm) {
				any_open = true;
				if (reg->no_update_ticks < 30)
					break;
			}
		}
		if (any_open) {
			any_alive = false;
			for (int r = 0; r < st->osd_region_count[s]; r++) {
				rvd_osd_region_t *reg = &st->osd_regions[s][r];
				if (reg->active && reg->shm && reg->no_update_ticks < 30) {
					any_alive = true;
					break;
				}
			}
		}

		for (int r = 0; r < st->osd_region_count[s]; r++) {
			rvd_osd_region_t *reg = &st->osd_regions[s][r];
			if (!reg->active || !reg->shm)
				continue;

			if (shm_is_stale(reg->shm, s, reg->name)) {
				RSS_INFO("osd %d/%s: producer gone, clearing", s, reg->name);
				rss_osd_close(reg->shm);
				reg->shm = NULL;
				reg->no_update_ticks = 0;
				if (reg->local_buf) {
					memset(reg->local_buf, 0, reg->width * reg->height * 4);
					push_region(st, s, reg);
				}
			}
		}
	}

push_updates:
	for (int s = 0; s < st->stream_count; s++) {
		if (st->streams[s].is_jpeg)
			continue;
		for (int r = 0; r < st->osd_region_count[s]; r++) {
			rvd_osd_region_t *reg = &st->osd_regions[s][r];
			if (!reg->active || !reg->shm)
				continue;

			/* Virtual region (merged into another) */
			if (reg->hal_handle < 0) {
				if (rss_osd_check_dirty(reg->shm))
					rss_osd_clear_dirty(reg->shm);
				continue;
			}

			if (!rss_osd_check_dirty(reg->shm)) {
				/* Check if merged uptime is dirty */
				if (strcmp(reg->name, "time") == 0) {
					rvd_osd_region_t *ureg =
						rvd_osd_find_region(st, s, "uptime");
					if (ureg && ureg->active && ureg->hal_handle < 0 &&
					    ureg->shm && rss_osd_check_dirty(ureg->shm)) {
						goto do_update;
					}
				}
				reg->no_update_ticks++;
				continue;
			}
		do_update:
			reg->no_update_ticks = 0;
			read_shm_and_push(st, s, reg);
			rss_osd_clear_dirty(reg->shm);

			/* Composite merged uptime into TIME region */
			if (strcmp(reg->name, "time") == 0) {
				rvd_osd_region_t *ureg = rvd_osd_find_region(st, s, "uptime");
				if (ureg && ureg->active && ureg->hal_handle < 0 && ureg->shm) {
					uint32_t uw, uh;
					const uint8_t *ubmp =
						rss_osd_get_active_buffer(ureg->shm, &uw, &uh);
					if (ubmp && uw > 0 && uh > 0 && uw <= reg->width &&
					    uh <= reg->height) {
						uint32_t x_off =
							reg->width > uw ? reg->width - uw : 0;
						uint32_t rows = uh < reg->height ? uh : reg->height;
						for (uint32_t row = 0; row < rows; row++)
							memcpy(reg->local_buf +
								       (row * reg->width + x_off) *
									       4,
							       ubmp + row * uw * 4, uw * 4);
					}
					rss_osd_clear_dirty(ureg->shm);
					push_region(st, s, reg);
				}
			}

			/* Privacy stays hidden unless active */
			if (!reg->shown && strcmp(reg->name, "privacy") == 0 && !st->privacy[s])
				continue;

			if (!reg->shown) {
				if (STREAM_ISP_OSD(st, s)) {
					int sensor = st->streams[s].sensor_idx;
					int stream_w = st->streams[s].enc_cfg.width;
					int stream_h = st->streams[s].enc_cfg.height;
					const char *pos_str =
						resolve_position(st->cfg, s, reg->name);
					int x, y;
					rvd_osd_calc_position(stream_w, stream_h, (int)reg->width,
							      (int)reg->height, pos_str, &x, &y);
					rss_osd_region_t attr = {
						.type = RSS_OSD_PIC,
						.x = x,
						.y = y,
						.width = (int)reg->width,
						.height = (int)reg->height,
						.bitmap_data = reg->local_buf,
						.bitmap_fmt = RSS_PIXFMT_BGRA,
					};
					int chx = st->streams[s].fs_chn % 3;
					RSS_HAL_CALL(st->ops, isp_osd_set_region_attr, st->hal_ctx,
						     sensor, reg->hal_handle, chx, &attr);
					RSS_HAL_CALL(st->ops, isp_osd_show_region, st->hal_ctx,
						     sensor, reg->hal_handle, 1);
				} else {
					int grp = st->streams[s].chn;
					RSS_HAL_CALL(st->ops, osd_show_region, st->hal_ctx,
						     reg->hal_handle, grp, 1, reg->layer + 1);
				}
				reg->shown = true;
			}
		}
	}

	pthread_mutex_unlock(&st->osd_lock);
}

/* ── Privacy ── */

static void set_privacy_stream(rvd_state_t *st, int s, bool enable)
{
	if (st->streams[s].is_jpeg)
		return;
	if (STREAM_ISP_OSD(st, s) && s > 0 &&
	    st->streams[s].sensor_idx == st->streams[s - 1].sensor_idx)
		return;

	if (STREAM_ISP_OSD(st, s)) {
		int sensor = st->streams[s].sensor_idx;
		int chx = st->streams[s].fs_chn % 3;
		int stream_w = st->streams[s].enc_cfg.width;
		int stream_h = st->streams[s].enc_cfg.height;
		uint32_t pcol = (uint32_t)strtoul(
			rss_config_get_str(st->cfg, "osd", "privacy_color", "0xFF000000"), NULL, 0);
		RSS_HAL_CALL(st->ops, isp_osd_set_mask, st->hal_ctx, sensor, chx, 0, enable ? 1 : 0,
			     0, 0, stream_w, stream_h, pcol);
	} else {
		int ph = st->privacy_handles[s];
		if (ph >= 0) {
			int grp = st->streams[s].chn;
			RSS_HAL_CALL(st->ops, osd_show_region, st->hal_ctx, ph, grp, enable ? 1 : 0,
				     0);
		}
	}

	/* Show/hide privacy text region */
	rvd_osd_region_t *preg = rvd_osd_find_region(st, s, "privacy");
	if (preg && preg->active && preg->hal_handle >= 0) {
		if (STREAM_ISP_OSD(st, s)) {
			int sensor = st->streams[s].sensor_idx;
			RSS_HAL_CALL(st->ops, isp_osd_show_region, st->hal_ctx, sensor,
				     preg->hal_handle, enable ? 1 : 0);
		} else {
			int grp = st->streams[s].chn;
			RSS_HAL_CALL(st->ops, osd_show_region, st->hal_ctx, preg->hal_handle, grp,
				     enable ? 1 : 0, preg->layer + 1);
		}
	}

	st->privacy[s] = enable;
}

void rvd_osd_set_privacy(rvd_state_t *st, bool enable, int stream)
{
	if (!st->osd_enabled)
		return;

	pthread_mutex_lock(&st->osd_lock);
	if (stream >= 0 && stream < st->stream_count) {
		set_privacy_stream(st, stream, enable);
		RSS_INFO("privacy stream %d %s", stream, enable ? "ON" : "OFF");
	} else {
		for (int s = 0; s < st->stream_count; s++)
			set_privacy_stream(st, s, enable);
		RSS_INFO("privacy %s (all streams)", enable ? "ON" : "OFF");
	}
	pthread_mutex_unlock(&st->osd_lock);
}

/* ── OSD thread ── */

void *rvd_osd_thread(void *arg)
{
	rvd_state_t *st = arg;
	RSS_INFO("osd update thread started");

	/* Show initial regions */
	pthread_mutex_lock(&st->osd_lock);
	for (int s = 0; s < st->stream_count; s++) {
		if (st->streams[s].is_jpeg)
			continue;
		for (int r = 0; r < st->osd_region_count[s]; r++) {
			rvd_osd_region_t *reg = &st->osd_regions[s][r];
			if (!reg->active || reg->hal_handle < 0)
				continue;
			if (strcmp(reg->name, "privacy") == 0)
				continue;
			if (STREAM_ISP_OSD(st, s)) {
				int sensor = st->streams[s].sensor_idx;
				int stream_w = st->streams[s].enc_cfg.width;
				int stream_h = st->streams[s].enc_cfg.height;
				const char *pos_str = resolve_position(st->cfg, s, reg->name);
				int x, y;
				rvd_osd_calc_position(stream_w, stream_h, (int)reg->width,
						      (int)reg->height, pos_str, &x, &y);
				rss_osd_region_t attr = {
					.type = RSS_OSD_PIC,
					.x = x,
					.y = y,
					.width = (int)reg->width,
					.height = (int)reg->height,
					.bitmap_data = reg->local_buf,
					.bitmap_fmt = RSS_PIXFMT_BGRA,
				};
				int chx = st->streams[s].fs_chn % 3;
				RSS_HAL_CALL(st->ops, isp_osd_set_region_attr, st->hal_ctx, sensor,
					     reg->hal_handle, chx, &attr);
				RSS_HAL_CALL(st->ops, isp_osd_show_region, st->hal_ctx, sensor,
					     reg->hal_handle, 1);
			} else {
				int grp = st->streams[s].chn;
				RSS_HAL_CALL(st->ops, osd_show_region, st->hal_ctx, reg->hal_handle,
					     grp, 1, reg->layer + 1);
			}
			reg->shown = true;
		}
	}
	pthread_mutex_unlock(&st->osd_lock);
	RSS_INFO("osd regions shown");

	while (*st->running) {
		rvd_osd_check(st);
		usleep(100000);
	}

	RSS_INFO("osd update thread exiting");
	return NULL;
}

/* ── Cleanup ── */

void rvd_osd_deinit_stream(rvd_state_t *st, int s)
{
	if (st->streams[s].is_jpeg)
		return;

	for (int r = 0; r < st->osd_region_count[s]; r++) {
		rvd_osd_region_t *reg = &st->osd_regions[s][r];
		if (reg->active && reg->hal_handle >= 0) {
			if (STREAM_ISP_OSD(st, s)) {
				int sensor = st->streams[s].sensor_idx;
				RSS_HAL_CALL(st->ops, isp_osd_show_region, st->hal_ctx, sensor,
					     reg->hal_handle, 0);
				RSS_HAL_CALL(st->ops, isp_osd_destroy_region, st->hal_ctx, sensor,
					     reg->hal_handle);
			} else {
				int grp = st->streams[s].chn;
				RSS_HAL_CALL(st->ops, osd_show_region, st->hal_ctx, reg->hal_handle,
					     grp, 0, reg->layer + 1);
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
		reg->shown = false;
	}
	st->osd_region_count[s] = 0;

	if (STREAM_ISP_OSD(st, s) && st->privacy[s]) {
		int sensor = st->streams[s].sensor_idx;
		int chx = st->streams[s].fs_chn % 3;
		RSS_HAL_CALL(st->ops, isp_osd_set_mask, st->hal_ctx, sensor, chx, 0, 0, 0, 0, 0, 0,
			     0);
	}
	int ph = st->privacy_handles[s];
	if (ph >= 0) {
		if (!STREAM_ISP_OSD(st, s)) {
			int grp = st->streams[s].chn;
			RSS_HAL_CALL(st->ops, osd_show_region, st->hal_ctx, ph, grp, 0, 0);
			RSS_HAL_CALL(st->ops, osd_unregister_region, st->hal_ctx, ph, grp);
			RSS_HAL_CALL(st->ops, osd_destroy_region, st->hal_ctx, ph);
		}
		st->privacy_handles[s] = -1;
	}
}


/*
 * rvd_osd_pool.c -- how many bytes of OSD region memory a config needs.
 *
 * Kept apart from the pipeline so the estimate can be exercised without
 * a HAL: the pool is fixed at HAL init, and a region it cannot hold gets
 * no memory and is never drawn.
 */

#include <stdbool.h>
#include <string.h>

#include "rvd_osd_pool.h"

/* Per-element region bytes of one [osd.*] section */
struct osd_pool_ctx {
	rss_config_t *cfg;
	int font_size;
	int main_w;
	int main_h;
	int sub_w;
	int sub_h;
	bool has_sub;
	bool has_detect;
	uint32_t total;
};

static void osd_pool_cb(const char *section, void *ud)
{
	struct osd_pool_ctx *c = ud;
	const char *dot = strchr(section, '.');
	if (!dot)
		return;
	if (strcmp(dot + 1, "detect") == 0)
		c->has_detect = true;
	const char *type = rss_config_get_str(c->cfg, section, "type", "text");
	int efs = rss_config_get_int(c->cfg, section, "font_size", 0);
	if (efs <= 0)
		efs = c->font_size;
	uint32_t adv = (uint32_t)(efs * 10 / 24);
	if (adv < 10)
		adv = 10;
	uint32_t eh = (uint32_t)(efs * 36 / 24);
	if (eh < 36)
		eh = 36;

	if (strcmp(type, "text") == 0) {
		int mc = rss_config_get_int(c->cfg, section, "max_chars", 20);
		uint32_t ew = mc * adv;
		c->total += ew * eh * 4;
		if (c->has_sub && c->main_w > 0 && c->main_h > 0) {
			uint32_t sew = ew * c->sub_w / c->main_w;
			uint32_t seh = eh * c->sub_h / c->main_h;
			if (seh < 16)
				seh = 16;
			c->total += sew * seh * 4;
		}
	} else if (strcmp(type, "image") == 0) {
		int iw = rss_config_get_int(c->cfg, section, "width", 100);
		int ih = rss_config_get_int(c->cfg, section, "height", 30);
		c->total += (uint32_t)(iw * ih * 4);
		if (c->has_sub)
			c->total += (uint32_t)(iw * ih * 4);
	} else if (strcmp(type, "receipt") == 0) {
		int ml = rss_config_get_int(c->cfg, section, "max_lines", 20);
		int mll = rss_config_get_int(c->cfg, section, "max_line_length", 80);
		uint32_t rw = mll * adv;
		uint32_t rh = ml * eh;
		c->total += rw * rh * 4;
		if (c->has_sub && c->main_w > 0 && c->main_h > 0) {
			uint32_t srw = rw * c->sub_w / c->main_w;
			uint32_t srh = rh * c->sub_h / c->main_h;
			c->total += srw * srh * 4;
		}
	} else if (strcmp(type, "overlay") == 0) {
		if (c->has_sub)
			c->total += (uint32_t)(c->sub_w * c->sub_h * 4);
	}
}

uint32_t rvd_osd_pool_bytes(rss_config_t *cfg)
{
	int font_size = rss_config_get_int(cfg, "osd", "font_size", 24);
	if (font_size < 10)
		font_size = 10;
	int main_w = rss_config_get_int(cfg, "stream0", "width", 2560);
	int main_h = rss_config_get_int(cfg, "stream0", "height", 1440);
	int sub_w = rss_config_get_int(cfg, "stream1", "width", 640);
	int sub_h = rss_config_get_int(cfg, "stream1", "height", 360);
	bool has_sub = rss_config_get_bool(cfg, "stream1", "enabled", true);

	struct osd_pool_ctx pctx = {
		.cfg = cfg,
		.font_size = font_size,
		.main_w = main_w,
		.main_h = main_h,
		.sub_w = sub_w,
		.sub_h = sub_h,
		.has_sub = has_sub,
		.has_detect = false,
		.total = 0,
	};
	rss_config_foreach_section(cfg, "osd.", osd_pool_cb, &pctx);

	/* With [motion] enabled ROD adds a full-frame "detect" overlay over
	 * the sub stream when no [osd.detect] section declares one
	 * (rod_config.c); its buffer comes from this pool too. */
	if (has_sub && !pctx.has_detect && rss_config_get_bool(cfg, "motion", "enabled", false))
		pctx.total += (uint32_t)(sub_w * sub_h * 4);

	return pctx.total;
}

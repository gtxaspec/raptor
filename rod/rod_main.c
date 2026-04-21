/*
 * rod_main.c -- Raptor OSD Daemon
 *
 * Dynamic element registry: renders named OSD elements into BGRA
 * bitmaps via SHM double-buffers for RVD consumption.
 *
 * ROD has no HAL dependency -- it is purely a bitmap producer.
 * RVD handles OSD group/region creation and hardware updates.
 *
 * Elements are created from the [osd] config section at startup
 * (backward compatible with the old fixed-slot format) and can
 * be added/removed at runtime via control socket commands.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/epoll.h>
#include <dirent.h>

#include "rod.h"

static uint32_t parse_color(const char *s)
{
	if (!s)
		return 0xFFFFFFFF;
	return (uint32_t)strtoul(s, NULL, 0);
}

/* ── Element registry ── */

rod_element_t *rod_find_element(rod_state_t *st, const char *name)
{
	for (int i = 0; i < st->elem_count; i++) {
		if (st->elements[i].active && strcmp(st->elements[i].name, name) == 0)
			return &st->elements[i];
	}
	return NULL;
}

int rod_alloc_font(rod_state_t *st, int stream_idx, int font_size)
{
	/* Reuse existing font context with same size */
	for (int i = 0; i < ROD_MAX_FONTS; i++) {
		if (st->fonts[stream_idx][i].refcount > 0 &&
		    st->fonts[stream_idx][i].size == font_size) {
			st->fonts[stream_idx][i].refcount++;
			return i;
		}
	}
	/* Allocate new slot */
	for (int i = 0; i < ROD_MAX_FONTS; i++) {
		if (st->fonts[stream_idx][i].refcount == 0) {
			if (rod_render_init(st, stream_idx, i, font_size) < 0)
				return -1;
			st->fonts[stream_idx][i].size = font_size;
			st->fonts[stream_idx][i].refcount = 1;
			return i;
		}
	}
	RSS_ERROR("font pool exhausted for stream %d", stream_idx);
	return -1;
}

static void release_font(rod_state_t *st, int stream_idx, int font_idx)
{
	if (font_idx < 0 || font_idx >= ROD_MAX_FONTS)
		return;
	rod_font_t *f = &st->fonts[stream_idx][font_idx];
	if (f->refcount > 0)
		f->refcount--;
	if (f->refcount == 0)
		rod_render_deinit(st, stream_idx, font_idx);
}

int rod_add_element(rod_state_t *st, const char *name, rod_elem_type_t type, const char *tmpl,
		    const char *position, int align, int font_size, int max_chars,
		    rod_update_mode_t update_mode)
{
	if (rod_find_element(st, name))
		return -1;
	if (st->elem_count >= ROD_MAX_ELEMENTS)
		return -1;

	rod_element_t *e = &st->elements[st->elem_count];
	memset(e, 0, sizeof(*e));
	rss_strlcpy(e->name, name, sizeof(e->name));
	e->type = type;
	e->active = true;
	e->visible = true;
	if (tmpl)
		rss_strlcpy(e->tmpl, tmpl, sizeof(e->tmpl));
	if (position)
		rss_strlcpy(e->position, position, sizeof(e->position));
	e->align = align;
	e->font_size = font_size;
	e->stroke_size = -1;
	e->max_chars = max_chars > 0 ? max_chars : 20;
	e->update_mode = update_mode;
	e->font_idx = -1;

	st->elem_count++;
	return 0;
}

void rod_remove_element(rod_state_t *st, const char *name)
{
	for (int i = 0; i < st->elem_count; i++) {
		if (!st->elements[i].active || strcmp(st->elements[i].name, name) != 0)
			continue;

		rod_element_t *e = &st->elements[i];
		for (int s = 0; s < st->stream_count; s++) {
			if (e->streams[s].shm) {
				rss_osd_destroy(e->streams[s].shm);
				e->streams[s].shm = NULL;
			}
			release_font(st, s, e->font_idx);
		}
		free(e->image_data);
		free(e->image_sub_data);
		e->active = false;
		return;
	}
}

/* ── Template variable expansion ── */

static void format_uptime(char *buf, size_t bufsz)
{
	FILE *f = fopen("/proc/uptime", "r");
	if (!f) {
		snprintf(buf, bufsz, "Up: ?");
		return;
	}
	double up = 0;
	if (fscanf(f, "%lf", &up) != 1)
		up = 0;
	fclose(f);

	int sec = (int)up;
	int days = sec / 86400;
	int hours = (sec % 86400) / 3600;
	int mins = (sec % 3600) / 60;
	int secs = sec % 60;

	if (days > 0)
		snprintf(buf, bufsz, "%dd %dh %dm %ds", days, hours, mins, secs);
	else if (hours > 0)
		snprintf(buf, bufsz, "%dh %dm %ds", hours, mins, secs);
	else
		snprintf(buf, bufsz, "%dm %ds", mins, secs);
}

int rod_expand_template(rod_state_t *st, const char *tmpl, char *out, int out_size)
{
	int pos = 0;
	const char *p = tmpl;

	while (*p && pos < out_size - 1) {
		if (*p != '%') {
			out[pos++] = *p++;
			continue;
		}

		const char *end = strchr(p + 1, '%');
		if (!end) {
			out[pos++] = *p++;
			continue;
		}

		int vlen = (int)(end - p - 1);
		if (vlen <= 0 || vlen > 31) {
			out[pos++] = *p++;
			continue;
		}

		char varname[32];
		memcpy(varname, p + 1, vlen);
		varname[vlen] = '\0';

		char val[128] = "";
		if (strcmp(varname, "time") == 0) {
			time_t now = time(NULL);
			struct tm tm;
			localtime_r(&now, &tm);
			strftime(val, sizeof(val), st->settings.time_format, &tm);
		} else if (strcmp(varname, "uptime") == 0) {
			format_uptime(val, sizeof(val));
		} else if (strcmp(varname, "hostname") == 0) {
			rss_strlcpy(val, st->hostname, sizeof(val));
		} else if (strcmp(varname, "text") == 0) {
			rss_strlcpy(val, st->settings.text_string, sizeof(val));
		} else {
			/* Custom variable lookup */
			for (int i = 0; i < st->var_count; i++) {
				if (strcmp(st->vars[i].name, varname) == 0) {
					rss_strlcpy(val, st->vars[i].value, sizeof(val));
					break;
				}
			}
		}

		int vallen = (int)strlen(val);
		if (pos + vallen < out_size) {
			memcpy(out + pos, val, vallen);
			pos += vallen;
		}
		p = end + 1;
	}

	out[pos] = '\0';
	return pos;
}

/* ── Config loading ── */

static void load_config(rod_state_t *st)
{
	rss_config_t *cfg = st->cfg;
	rod_config_t *c = &st->settings;

	c->enabled = rss_config_get_bool(cfg, "osd", "enabled", true);
	rss_strlcpy(c->font_path,
		    rss_config_get_str(cfg, "osd", "font", "/usr/share/fonts/default.ttf"),
		    sizeof(c->font_path));
	c->font_size = rss_config_get_int(cfg, "osd", "font_size", 24);
	c->time_font_size = rss_config_get_int(cfg, "osd", "time_font_size", 0);
	c->uptime_font_size = rss_config_get_int(cfg, "osd", "uptime_font_size", 0);
	c->text_font_size = rss_config_get_int(cfg, "osd", "text_font_size", 0);
	c->font_color = parse_color(rss_config_get_str(cfg, "osd", "font_color", "0xFFFFFFFF"));
	c->stroke_color = parse_color(rss_config_get_str(cfg, "osd", "stroke_color", "0xFF000000"));
	c->font_stroke = rss_config_get_int(cfg, "osd", "font_stroke", 1);

	c->time_enabled = rss_config_get_bool(cfg, "osd", "time_enabled", true);
	rss_strlcpy(c->time_format,
		    rss_config_get_str(cfg, "osd", "time_format", "%Y-%m-%d %H:%M:%S"),
		    sizeof(c->time_format));

	c->uptime_enabled = rss_config_get_bool(cfg, "osd", "uptime_enabled", true);

	c->text_enabled = rss_config_get_bool(cfg, "osd", "text_enabled", true);
	rss_strlcpy(c->text_string, rss_config_get_str(cfg, "osd", "text_string", "Camera"),
		    sizeof(c->text_string));

	c->logo_enabled = rss_config_get_bool(cfg, "osd", "logo_enabled", true);
	rss_strlcpy(c->logo_path,
		    rss_config_get_str(cfg, "osd", "logo_path",
				       "/usr/share/images/thingino_100x30.bgra"),
		    sizeof(c->logo_path));
	c->logo_width = rss_config_get_int(cfg, "osd", "logo_width", 100);
	c->logo_height = rss_config_get_int(cfg, "osd", "logo_height", 30);

	st->stream_w[0] = rss_config_get_int(cfg, "stream0", "width", 1920);
	st->stream_h[0] = rss_config_get_int(cfg, "stream0", "height", 1080);
	st->stream_count = 1;

	if (rss_config_get_bool(cfg, "stream1", "enabled", true)) {
		st->stream_w[1] = rss_config_get_int(cfg, "stream1", "width", 640);
		st->stream_h[1] = rss_config_get_int(cfg, "stream1", "height", 360);
		st->stream_count = 2;
	}

	static const char *sensor_sections[] = {"sensor1_stream0", "sensor1_stream1",
						"sensor2_stream0", "sensor2_stream1"};
	for (int i = 0; i < 4 && st->stream_count < ROD_MAX_STREAMS; i++) {
		const char *sec = sensor_sections[i];
		if (rss_config_get_str(cfg, sec, "codec", "")[0] != '\0' ||
		    rss_config_get_bool(cfg, sec, "enabled", false)) {
			int s = st->stream_count;
			st->stream_w[s] = rss_config_get_int(cfg, sec, "width",
							     (i % 2 == 0) ? st->stream_w[0]
									  : st->stream_w[1]);
			st->stream_h[s] = rss_config_get_int(cfg, sec, "height",
							     (i % 2 == 0) ? st->stream_h[0]
									  : st->stream_h[1]);
			st->stream_count++;
		}
	}

	st->detect_enabled = rss_config_get_bool(cfg, "motion", "enabled", false);
	gethostname(st->hostname, sizeof(st->hostname));
}

/* ── New [osd.*] section format ── */

static int parse_align(const char *s)
{
	if (!s || !s[0])
		return 0;
	if (strcmp(s, "right") == 0)
		return 2;
	if (strcmp(s, "center") == 0)
		return 1;
	return 0;
}

struct osd_section_ctx {
	rod_state_t *st;
	int count;
};

static void load_osd_section(const char *section, void *userdata)
{
	struct osd_section_ctx *ctx = userdata;
	rod_state_t *st = ctx->st;
	rss_config_t *cfg = st->cfg;

	/* Extract element name from "osd.name" */
	const char *dot = strchr(section, '.');
	if (!dot)
		return;
	const char *name = dot + 1;
	if (!name[0] || strlen(name) >= ROD_ELEM_NAME_LEN)
		return;

	const char *type_str = rss_config_get_str(cfg, section, "type", "text");
	const char *tmpl = rss_config_get_str(cfg, section, "template", "");
	const char *position = rss_config_get_str(cfg, section, "position", "top_left");
	const char *align_str = rss_config_get_str(cfg, section, "align", "");
	int font_size = rss_config_get_int(cfg, section, "font_size", 0);
	int max_chars = rss_config_get_int(cfg, section, "max_chars", 20);
	bool visible = rss_config_get_bool(cfg, section, "visible", true);
	bool sub_only = rss_config_get_bool(cfg, section, "sub_only", false);
	const char *update_str = rss_config_get_str(cfg, section, "update", "tick");

	rod_elem_type_t type = ROD_ELEM_TEXT;
	if (strcmp(type_str, "image") == 0)
		type = ROD_ELEM_IMAGE;
	else if (strcmp(type_str, "overlay") == 0)
		type = ROD_ELEM_OVERLAY;

	int align = parse_align(align_str);
	rod_update_mode_t update = ROD_UPDATE_TICK;
	if (strcmp(update_str, "change") == 0)
		update = ROD_UPDATE_CHANGE;

	if (rod_add_element(st, name, type, tmpl, position, align, font_size, max_chars, update) <
	    0)
		return;

	rod_element_t *e = rod_find_element(st, name);
	if (!e)
		return;

	e->visible = visible;
	e->sub_streams_only = sub_only;

	if (type == ROD_ELEM_IMAGE) {
		const char *path = rss_config_get_str(cfg, section, "path", "");
		if (path[0])
			rss_strlcpy(e->image_path, path, sizeof(e->image_path));
		e->image_w = rss_config_get_int(cfg, section, "width", 0);
		e->image_h = rss_config_get_int(cfg, section, "height", 0);
	}

	int stroke = rss_config_get_int(cfg, section, "stroke_size", -1);
	if (stroke >= 0)
		e->stroke_size = stroke;

	const char *color_str = rss_config_get_str(cfg, section, "color", NULL);
	if (color_str)
		e->color = (uint32_t)strtoul(color_str, NULL, 0);

	const char *sc_str = rss_config_get_str(cfg, section, "stroke_color", NULL);
	if (sc_str)
		e->stroke_color = (uint32_t)strtoul(sc_str, NULL, 0);

	ctx->count++;
	RSS_DEBUG("osd element from config: [%s] name=%s type=%s", section, name, type_str);
}

/*
 * Populate the element registry from config.
 * New format: [osd.name] sections take priority.
 * Old format: time_enabled/text_string keys as fallback.
 */
static void init_elements_from_config(rod_state_t *st)
{
	/* Check for new [osd.*] sections */
	struct osd_section_ctx ctx = {.st = st, .count = 0};
	rss_config_foreach_section(st->cfg, "osd.", load_osd_section, &ctx);

	if (ctx.count > 0) {
		RSS_INFO("loaded %d OSD elements from [osd.*] config sections", ctx.count);

		/* Always add privacy if not defined in config */
		if (!rod_find_element(st, "privacy")) {
			rod_add_element(st, "privacy", ROD_ELEM_TEXT, "Privacy Mode", "center", 1,
					0, 20, ROD_UPDATE_CHANGE);
			rod_element_t *e = rod_find_element(st, "privacy");
			if (e)
				e->visible = false;
		}
		return;
	}

	/* Fall back to old [osd] keys */
	rod_config_t *c = &st->settings;

	if (c->time_enabled) {
		rod_add_element(st, "time", ROD_ELEM_TEXT, "%time%", "top_left", 0,
				c->time_font_size, 20, ROD_UPDATE_TICK);
	}

	if (c->uptime_enabled) {
		rod_add_element(st, "uptime", ROD_ELEM_TEXT, "%uptime%", "top_right", 2,
				c->uptime_font_size, 16, ROD_UPDATE_TICK);
	}

	if (c->text_enabled) {
		rod_add_element(st, "text", ROD_ELEM_TEXT, c->text_string, "top_center", 1,
				c->text_font_size, 14, ROD_UPDATE_CHANGE);
	}

	if (c->logo_enabled) {
		rod_add_element(st, "logo", ROD_ELEM_IMAGE, NULL, "bottom_right", 0, 0, 0,
				ROD_UPDATE_CHANGE);
		rod_element_t *e = rod_find_element(st, "logo");
		if (e) {
			rss_strlcpy(e->image_path, c->logo_path, sizeof(e->image_path));
			e->image_w = c->logo_width;
			e->image_h = c->logo_height;
		}
	}

	rod_add_element(st, "privacy", ROD_ELEM_TEXT, "Privacy Mode", "center", 1, 0, 20,
			ROD_UPDATE_CHANGE);
	{
		rod_element_t *e = rod_find_element(st, "privacy");
		if (e)
			e->visible = false;
	}

	if (st->detect_enabled) {
		rod_add_element(st, "detect", ROD_ELEM_OVERLAY, NULL, "0,0", 0, 0, 0,
				ROD_UPDATE_TICK);
		rod_element_t *e = rod_find_element(st, "detect");
		if (e)
			e->sub_streams_only = true;
	}
}

/* ── SHM creation ── */

static void font_region_dims(rod_font_t *f, int chars, int stroke, uint32_t *w, uint32_t *h)
{
	int adv = f->max_text_width / 24;
	if (adv < 10)
		adv = 10;
	int pad = stroke > 0 ? stroke * 2 : 0;
	*w = chars * adv + pad;
	*h = f->text_height;
}

static void create_elem_shm(rod_state_t *st, rod_element_t *e, int s)
{
	char name[64];
	snprintf(name, sizeof(name), "osd_%d_%s", s, e->name);

	uint32_t w, h;

	if (e->type == ROD_ELEM_TEXT) {
		if (e->font_idx < 0)
			return;
		int stroke = e->stroke_size >= 0 ? e->stroke_size : st->settings.font_stroke;
		font_region_dims(&st->fonts[s][e->font_idx], e->max_chars, stroke, &w, &h);
	} else if (e->type == ROD_ELEM_IMAGE) {
		if (s == 0) {
			w = e->image_w;
			h = e->image_h;
		} else {
			w = e->image_sub_w > 0 ? e->image_sub_w : e->image_w;
			h = e->image_sub_h > 0 ? e->image_sub_h : e->image_h;
		}
		if (w == 0 || h == 0)
			return;
	} else if (e->type == ROD_ELEM_OVERLAY) {
		w = st->stream_w[s];
		h = st->stream_h[s];
	} else {
		return;
	}

	w = (w + 1) & ~1u;
	h = (h + 1) & ~1u;

	rss_osd_shm_t *shm = rss_osd_create(name, w, h);
	if (!shm) {
		RSS_WARN("failed to create OSD SHM: %s", name);
		return;
	}

	e->streams[s].shm = shm;
	e->streams[s].width = w;
	e->streams[s].height = h;
	e->streams[s].needs_update = true;

	RSS_INFO("osd shm %s: %ux%u", name, w, h);
}

static void create_all_shms(rod_state_t *st)
{
	for (int i = 0; i < st->elem_count; i++) {
		rod_element_t *e = &st->elements[i];
		if (!e->active)
			continue;

		for (int s = 0; s < st->stream_count; s++) {
			if (e->sub_streams_only && s == 0)
				continue;
			create_elem_shm(st, e, s);
		}
	}
}

/* ── Rendering ── */

static void render_text_element(rod_state_t *st, rod_element_t *e, int s, const char *text)
{
	rod_elem_stream_t *es = &e->streams[s];
	if (!es->shm)
		return;

	uint8_t *buf = rss_osd_get_draw_buffer(es->shm);
	if (!buf)
		return;

	rod_draw_text(st, s, e->font_idx, buf, es->width, es->height, text, e->align);
	rss_osd_publish(es->shm);
}

static void render_image_element(rod_state_t *st, rod_element_t *e, int s)
{
	(void)st;
	rod_elem_stream_t *es = &e->streams[s];
	if (!es->shm)
		return;

	uint8_t *img = (s == 0) ? e->image_data : e->image_sub_data;
	int img_w = (s == 0) ? e->image_w : e->image_sub_w;
	int img_h = (s == 0) ? e->image_h : e->image_sub_h;
	if (!img)
		return;

	uint8_t *buf = rss_osd_get_draw_buffer(es->shm);
	if (!buf)
		return;

	memset(buf, 0, es->width * es->height * 4);
	for (int row = 0; row < img_h && row < (int)es->height; row++)
		memcpy(buf + row * es->width * 4, img + row * img_w * 4, img_w * 4);

	rss_osd_publish(es->shm);
	es->needs_update = false;
}

static void render_detections(rod_state_t *st, rod_element_t *e)
{
	char resp[2048];
	int ret = rss_ctrl_send_command("/var/run/rss/rvd.sock", "{\"cmd\":\"ivs-detections\"}",
					resp, sizeof(resp), 500);
	if (ret < 0)
		return;

	int count = 0;
	rss_json_get_int(resp, "count", &count);
	if (count < 0)
		count = 0;
	if (count > 20)
		count = 20;

	for (int s = 0; s < st->stream_count; s++) {
		if (e->sub_streams_only && s == 0)
			continue;
		rod_elem_stream_t *es = &e->streams[s];
		if (!es->shm)
			continue;

		uint8_t *buf = rss_osd_get_draw_buffer(es->shm);
		if (!buf)
			continue;

		memset(buf, 0, es->width * es->height * 4);

		if (count > 0) {
			cJSON *root = cJSON_Parse(resp);
			if (!root)
				goto publish;
			cJSON *dets = cJSON_GetObjectItem(root, "detections");
			cJSON *det;
			cJSON_ArrayForEach(det, dets)
			{
				int x0 = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(det, "x0"));
				int y0 = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(det, "y0"));
				int x1 = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(det, "x1"));
				int y1 = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(det, "y1"));
				rod_draw_rect_outline(buf, es->width, es->height, x0, y0, x1, y1,
						      0xFF00FF00, 2);
			}
			cJSON_Delete(root);
		}
	publish:
		rss_osd_publish(es->shm);
	}
}

static void render_tick(rod_state_t *st)
{
	for (int i = 0; i < st->elem_count; i++) {
		rod_element_t *e = &st->elements[i];
		if (!e->active || !e->visible)
			continue;

		if (e->type == ROD_ELEM_TEXT) {
			char expanded[ROD_EXPANDED_LEN];
			rod_expand_template(st, e->tmpl, expanded, sizeof(expanded));

			bool changed = strcmp(expanded, e->last_expanded) != 0;
			if (changed)
				rss_strlcpy(e->last_expanded, expanded, sizeof(e->last_expanded));

			bool should_render = (e->update_mode == ROD_UPDATE_TICK) ||
					     (changed && e->update_mode == ROD_UPDATE_CHANGE);

			if (!should_render) {
				/* Still need to check per-stream needs_update for
				 * initial render or after SHM recreate */
				for (int s = 0; s < st->stream_count; s++) {
					if (e->streams[s].needs_update && e->streams[s].shm) {
						render_text_element(st, e, s, expanded);
						e->streams[s].needs_update = false;
					}
				}
				continue;
			}

			for (int s = 0; s < st->stream_count; s++) {
				if (e->sub_streams_only && s == 0)
					continue;
				render_text_element(st, e, s, expanded);
				e->streams[s].needs_update = false;
			}
		} else if (e->type == ROD_ELEM_IMAGE) {
			for (int s = 0; s < st->stream_count; s++) {
				if (e->sub_streams_only && s == 0)
					continue;
				if (e->streams[s].needs_update)
					render_image_element(st, e, s);
			}
		}
		/* OVERLAY handled separately at different cadence */
	}

	/* Heartbeat: touch one active SHM per stream so RVD knows we're alive */
	for (int s = 0; s < st->stream_count; s++) {
		for (int i = 0; i < st->elem_count; i++) {
			rod_element_t *e = &st->elements[i];
			if (e->active && e->streams[s].shm) {
				rss_osd_heartbeat(e->streams[s].shm);
				break;
			}
		}
	}
}

/* ── Control socket ── */

static void mark_all_dirty(rod_state_t *st)
{
	for (int i = 0; i < st->elem_count; i++) {
		if (!st->elements[i].active)
			continue;
		for (int s = 0; s < st->stream_count; s++)
			st->elements[i].streams[s].needs_update = true;
	}
}

static void mark_element_dirty(rod_element_t *e, int stream_count)
{
	for (int s = 0; s < stream_count; s++)
		e->streams[s].needs_update = true;
}

static int handle_set_text(rod_state_t *st, const char *cmd_json, char *resp, int resp_size)
{
	char val[128];
	if (rss_json_get_str(cmd_json, "value", val, sizeof(val)) != 0)
		return rss_ctrl_resp_error(resp, resp_size, "missing value");

	for (int i = 0; val[i]; i++) {
		if ((unsigned char)val[i] < 0x20)
			val[i] = ' ';
	}

	rss_strlcpy(st->settings.text_string, val, sizeof(st->settings.text_string));
	rss_config_set_str(st->cfg, "osd", "text_string", val);

	/* Update the "text" element's template if it's a literal (no % vars) */
	rod_element_t *e = rod_find_element(st, "text");
	if (e && !strchr(e->tmpl, '%')) {
		rss_strlcpy(e->tmpl, val, sizeof(e->tmpl));
		mark_element_dirty(e, st->stream_count);
	}
	return rss_ctrl_resp_ok(resp, resp_size);
}

static int handle_color_change(rod_state_t *st, const char *cmd_json, char *resp, int resp_size,
			       const char *key, uint32_t *target)
{
	char val[16];
	if (rss_json_get_str(cmd_json, "value", val, sizeof(val)) != 0)
		return rss_ctrl_resp_error(resp, resp_size, "missing value");

	*target = parse_color(val);
	rss_config_set_str(st->cfg, "osd", key, val);
	mark_all_dirty(st);
	return rss_ctrl_resp_ok(resp, resp_size);
}

static int handle_enable(rod_state_t *st, const char *cmd_json, char *resp, int resp_size,
			 const char *cmd)
{
	int val;
	if (rss_json_get_int(cmd_json, "value", &val) != 0)
		return rss_ctrl_resp_error(resp, resp_size, "need value (0/1)");

	bool enable = !!val;
	const char *elem_name;
	bool *setting;
	const char *key;

	if (strcmp(cmd, "enable-time") == 0) {
		elem_name = "time";
		setting = &st->settings.time_enabled;
		key = "time_enabled";
	} else if (strcmp(cmd, "enable-uptime") == 0) {
		elem_name = "uptime";
		setting = &st->settings.uptime_enabled;
		key = "uptime_enabled";
	} else if (strcmp(cmd, "enable-logo") == 0) {
		elem_name = "logo";
		setting = &st->settings.logo_enabled;
		key = "logo_enabled";
	} else {
		elem_name = "text";
		setting = &st->settings.text_enabled;
		key = "text_enabled";
	}

	int target_stream = -1;
	rss_json_get_int(cmd_json, "stream", &target_stream);

	if (target_stream < 0) {
		if (*setting == enable)
			return rss_ctrl_resp_ok(resp, resp_size);
		*setting = enable;
		rss_config_set_str(st->cfg, "osd", key, enable ? "true" : "false");
	}

	/* Tell RVD to show/hide the HAL region */
	for (int s = 0; s < st->stream_count; s++) {
		if (target_stream >= 0 && s != target_stream)
			continue;
		char fwd[128];
		{
			cJSON *j = cJSON_CreateObject();
			if (!j)
				continue;
			cJSON_AddStringToObject(j, "cmd", "osd-show");
			cJSON_AddNumberToObject(j, "stream", s);
			cJSON_AddStringToObject(j, "region", elem_name);
			cJSON_AddNumberToObject(j, "show", enable ? 1 : 0);
			cJSON_PrintPreallocated(j, fwd, sizeof(fwd), 0);
			cJSON_Delete(j);
		}
		char rvd_resp[256];
		rss_ctrl_send_command("/var/run/rss/rvd.sock", fwd, rvd_resp, sizeof(rvd_resp),
				      1000);
	}

	RSS_INFO("%s: %s", key, enable ? "on" : "off");
	return rss_ctrl_resp_ok(resp, resp_size);
}

static int handle_set_position(rod_state_t *st, const char *cmd_json, char *resp, int resp_size)
{
	char element[32] = "", pos[32] = "";
	rss_json_get_str(cmd_json, "element", element, sizeof(element));
	rss_json_get_str(cmd_json, "pos", pos, sizeof(pos));

	if (!element[0] || !pos[0])
		return rss_ctrl_resp_error(resp, resp_size, "need element and pos");

	int target_stream = -1;
	rss_json_get_int(cmd_json, "stream", &target_stream);
	for (int s = 0; s < st->stream_count; s++) {
		if (target_stream >= 0 && s != target_stream)
			continue;
		char fwd[128];
		{
			cJSON *j = cJSON_CreateObject();
			if (!j)
				continue;
			cJSON_AddStringToObject(j, "cmd", "osd-position");
			cJSON_AddNumberToObject(j, "stream", s);
			cJSON_AddStringToObject(j, "region", element);
			cJSON_AddStringToObject(j, "pos", pos);
			cJSON_PrintPreallocated(j, fwd, sizeof(fwd), 0);
			cJSON_Delete(j);
		}
		char rvd_resp[256];
		rss_ctrl_send_command("/var/run/rss/rvd.sock", fwd, rvd_resp, sizeof(rvd_resp),
				      1000);
	}

	RSS_INFO("set-position: %s -> %s", element, pos);
	return rss_ctrl_resp_ok(resp, resp_size);
}

static int handle_font_size_change(rod_state_t *st, const char *cmd_json, char *resp, int resp_size,
				   const char *cmd)
{
	int val;
	if (rss_json_get_int(cmd_json, "value", &val) != 0 || val < 10 || val > 72)
		return rss_ctrl_resp_error(resp, resp_size, "need value 10-72");

	bool is_global = strcmp(cmd, "set-font-size") == 0;
	const char *elem_name = NULL;
	int *setting = NULL;
	const char *key = NULL;

	if (!is_global) {
		if (strcmp(cmd, "set-time-font-size") == 0) {
			elem_name = "time";
			setting = &st->settings.time_font_size;
			key = "time_font_size";
		} else if (strcmp(cmd, "set-uptime-font-size") == 0) {
			elem_name = "uptime";
			setting = &st->settings.uptime_font_size;
			key = "uptime_font_size";
		} else {
			elem_name = "text";
			setting = &st->settings.text_font_size;
			key = "text_font_size";
		}
	}

	if (is_global) {
		RSS_INFO("set-font-size: %d -> %d", st->settings.font_size, val);
		st->settings.font_size = val;
		rss_config_set_int(st->cfg, "osd", "font_size", val);
	} else {
		RSS_INFO("set-%s: -> %d", key, val);
		*setting = val;
		rss_config_set_int(st->cfg, "osd", key, val);
	}

	/* Rebuild font contexts and SHMs for affected elements */
	for (int i = 0; i < st->elem_count; i++) {
		rod_element_t *e = &st->elements[i];
		if (!e->active || e->type != ROD_ELEM_TEXT)
			continue;
		if (!is_global && elem_name && strcmp(e->name, elem_name) != 0)
			continue;

		int new_size = e->font_size > 0 ? e->font_size : st->settings.font_size;
		if (!is_global && elem_name && strcmp(e->name, elem_name) == 0)
			new_size = val;

		for (int s = 0; s < st->stream_count; s++) {
			int fs = new_size;
			if (s > 0 && st->stream_h[0] > 0) {
				fs = fs * st->stream_h[s] / st->stream_h[0];
				if (fs < 12)
					fs = 12;
			}

			release_font(st, s, e->font_idx);
			int fi = rod_alloc_font(st, s, fs);
			if (fi < 0) {
				RSS_ERROR("font alloc failed s%d elem %s", s, e->name);
				continue;
			}
			e->font_idx = fi;

			if (e->streams[s].shm) {
				rss_osd_destroy(e->streams[s].shm);
				e->streams[s].shm = NULL;
			}
			create_elem_shm(st, e, s);
		}
	}

	if (!is_global && elem_name) {
		rod_element_t *target = rod_find_element(st, elem_name);
		if (target)
			target->font_size = val;
	}

	/* Notify RVD to pick up new SHM dimensions */
	{
		char fwd[96];
		cJSON *j = cJSON_CreateObject();
		if (j) {
			cJSON_AddStringToObject(j, "cmd", "osd-restart");
			cJSON_AddNumberToObject(j, "pool_kb", 0);
			if (is_global)
				cJSON_AddNumberToObject(j, "font_size", val);
			cJSON_PrintPreallocated(j, fwd, sizeof(fwd), 0);
			cJSON_Delete(j);
		}
		char rvd_resp[256];
		rss_ctrl_send_command("/var/run/rss/rvd.sock", fwd, rvd_resp, sizeof(rvd_resp),
				      5000);
	}

	return rss_ctrl_resp_ok(resp, resp_size);
}

static int handle_elements_list(rod_state_t *st, char *resp, int resp_size)
{
	static const char *type_names[] = {"text", "image", "overlay"};

	cJSON *r = cJSON_CreateObject();
	if (!r)
		return rss_ctrl_resp_error(resp, resp_size, "alloc");
	cJSON_AddStringToObject(r, "status", "ok");
	cJSON *arr = cJSON_AddArrayToObject(r, "elements");
	for (int i = 0; i < st->elem_count; i++) {
		rod_element_t *e = &st->elements[i];
		if (!e->active)
			continue;
		cJSON *obj = cJSON_CreateObject();
		if (!obj)
			continue;
		cJSON_AddStringToObject(obj, "name", e->name);
		cJSON_AddStringToObject(obj, "type", type_names[e->type]);
		cJSON_AddBoolToObject(obj, "visible", e->visible);
		cJSON_AddStringToObject(obj, "position", e->position);
		if (e->type == ROD_ELEM_TEXT)
			cJSON_AddStringToObject(obj, "template", e->tmpl);
		int fs = e->font_size > 0 ? e->font_size : st->settings.font_size;
		cJSON_AddNumberToObject(obj, "font_size", fs);
		cJSON_AddItemToArray(arr, obj);
	}
	return rss_ctrl_resp_json(resp, resp_size, r);
}

static int handle_add_element(rod_state_t *st, const char *cmd_json, char *resp, int resp_size)
{
	char name[ROD_ELEM_NAME_LEN] = "";
	char type_str[16] = "text";
	char tmpl[ROD_TMPL_LEN] = "";
	char position[32] = "top_left";
	int font_size = 0, max_chars = 20, align = 0;

	rss_json_get_str(cmd_json, "name", name, sizeof(name));
	rss_json_get_str(cmd_json, "type", type_str, sizeof(type_str));
	rss_json_get_str(cmd_json, "template", tmpl, sizeof(tmpl));
	rss_json_get_str(cmd_json, "position", position, sizeof(position));
	rss_json_get_int(cmd_json, "font_size", &font_size);
	rss_json_get_int(cmd_json, "max_chars", &max_chars);
	rss_json_get_int(cmd_json, "align", &align);

	if (!name[0])
		return rss_ctrl_resp_error(resp, resp_size, "need name");

	rod_elem_type_t type = ROD_ELEM_TEXT;
	if (strcmp(type_str, "image") == 0)
		type = ROD_ELEM_IMAGE;
	else if (strcmp(type_str, "overlay") == 0)
		type = ROD_ELEM_OVERLAY;

	int ret = rod_add_element(st, name, type, tmpl, position, align, font_size, max_chars,
				  ROD_UPDATE_TICK);
	if (ret < 0)
		return rss_ctrl_resp_error(resp, resp_size, "add failed (exists or full)");

	rod_element_t *e = rod_find_element(st, name);
	if (!e)
		return rss_ctrl_resp_error(resp, resp_size, "internal error");

	/* Allocate fonts and SHMs */
	if (e->type == ROD_ELEM_TEXT) {
		for (int s = 0; s < st->stream_count; s++) {
			int fs = e->font_size > 0 ? e->font_size : st->settings.font_size;
			if (s > 0 && st->stream_h[0] > 0) {
				fs = fs * st->stream_h[s] / st->stream_h[0];
				if (fs < 12)
					fs = 12;
			}
			int fi = rod_alloc_font(st, s, fs);
			if (fi >= 0) {
				e->font_idx = fi;
				create_elem_shm(st, e, s);
			}
		}
	}

	/* Tell RVD the position for this element (saved in RVD's config
	 * so scan_new_shm uses it when creating the HAL region) */
	for (int s = 0; s < st->stream_count; s++) {
		char fwd[128];
		cJSON *j = cJSON_CreateObject();
		if (!j)
			continue;
		cJSON_AddStringToObject(j, "cmd", "osd-position");
		cJSON_AddNumberToObject(j, "stream", s);
		cJSON_AddStringToObject(j, "region", name);
		cJSON_AddStringToObject(j, "pos", position);
		cJSON_PrintPreallocated(j, fwd, sizeof(fwd), 0);
		cJSON_Delete(j);
		char rvd_resp[256];
		rss_ctrl_send_command("/var/run/rss/rvd.sock", fwd, rvd_resp, sizeof(rvd_resp),
				      1000);
	}

	RSS_INFO("add-element: %s type=%s template=\"%s\" pos=%s", name, type_str, tmpl, position);
	return rss_ctrl_resp_ok(resp, resp_size);
}

static int handle_remove_element(rod_state_t *st, const char *cmd_json, char *resp, int resp_size)
{
	char name[ROD_ELEM_NAME_LEN] = "";
	rss_json_get_str(cmd_json, "name", name, sizeof(name));
	if (!name[0])
		return rss_ctrl_resp_error(resp, resp_size, "need name");

	if (!rod_find_element(st, name))
		return rss_ctrl_resp_error(resp, resp_size, "not found");

	rod_remove_element(st, name);
	RSS_INFO("remove-element: %s", name);
	return rss_ctrl_resp_ok(resp, resp_size);
}

static int handle_set_element(rod_state_t *st, const char *cmd_json, char *resp, int resp_size)
{
	char name[ROD_ELEM_NAME_LEN] = "";
	rss_json_get_str(cmd_json, "name", name, sizeof(name));
	if (!name[0])
		return rss_ctrl_resp_error(resp, resp_size, "need name");

	rod_element_t *e = rod_find_element(st, name);
	if (!e)
		return rss_ctrl_resp_error(resp, resp_size, "not found");

	char val[ROD_TMPL_LEN];
	if (rss_json_get_str(cmd_json, "template", val, sizeof(val)) == 0) {
		rss_strlcpy(e->tmpl, val, sizeof(e->tmpl));
		e->last_expanded[0] = '\0';
		mark_element_dirty(e, st->stream_count);
	}

	if (rss_json_get_str(cmd_json, "position", val, sizeof(val)) == 0) {
		rss_strlcpy(e->position, val, sizeof(e->position));
		for (int s = 0; s < st->stream_count; s++) {
			char fwd[128];
			cJSON *j = cJSON_CreateObject();
			if (!j)
				continue;
			cJSON_AddStringToObject(j, "cmd", "osd-position");
			cJSON_AddNumberToObject(j, "stream", s);
			cJSON_AddStringToObject(j, "region", name);
			cJSON_AddStringToObject(j, "pos", val);
			cJSON_PrintPreallocated(j, fwd, sizeof(fwd), 0);
			cJSON_Delete(j);
			char rvd_resp[256];
			rss_ctrl_send_command("/var/run/rss/rvd.sock", fwd, rvd_resp,
					      sizeof(rvd_resp), 1000);
		}
	}

	int ival;
	if (rss_json_get_int(cmd_json, "align", &ival) == 0 && ival >= 0 && ival <= 2) {
		e->align = ival;
		mark_element_dirty(e, st->stream_count);
	}

	if (rss_json_get_str(cmd_json, "color", val, sizeof(val)) == 0) {
		e->color = (uint32_t)strtoul(val, NULL, 0);
		mark_element_dirty(e, st->stream_count);
	}

	return rss_ctrl_resp_ok(resp, resp_size);
}

static int handle_set_var(rod_state_t *st, const char *cmd_json, char *resp, int resp_size)
{
	char name[32] = "", value[128] = "";
	rss_json_get_str(cmd_json, "name", name, sizeof(name));
	rss_json_get_str(cmd_json, "value", value, sizeof(value));
	if (!name[0])
		return rss_ctrl_resp_error(resp, resp_size, "need name");

	for (int i = 0; i < st->var_count; i++) {
		if (strcmp(st->vars[i].name, name) == 0) {
			rss_strlcpy(st->vars[i].value, value, sizeof(st->vars[i].value));
			return rss_ctrl_resp_ok(resp, resp_size);
		}
	}
	if (st->var_count >= ROD_MAX_VARS)
		return rss_ctrl_resp_error(resp, resp_size, "var limit reached");

	rss_strlcpy(st->vars[st->var_count].name, name, sizeof(st->vars[0].name));
	rss_strlcpy(st->vars[st->var_count].value, value, sizeof(st->vars[0].value));
	st->var_count++;
	return rss_ctrl_resp_ok(resp, resp_size);
}

static int handle_show_hide(rod_state_t *st, const char *cmd_json, char *resp, int resp_size,
			    bool show)
{
	char name[ROD_ELEM_NAME_LEN] = "";
	rss_json_get_str(cmd_json, "name", name, sizeof(name));
	if (!name[0])
		return rss_ctrl_resp_error(resp, resp_size, "need name");

	rod_element_t *e = rod_find_element(st, name);
	if (!e)
		return rss_ctrl_resp_error(resp, resp_size, "not found");

	e->visible = show;

	for (int s = 0; s < st->stream_count; s++) {
		char fwd[128];
		cJSON *j = cJSON_CreateObject();
		if (!j)
			continue;
		cJSON_AddStringToObject(j, "cmd", "osd-show");
		cJSON_AddNumberToObject(j, "stream", s);
		cJSON_AddStringToObject(j, "region", name);
		cJSON_AddNumberToObject(j, "show", show ? 1 : 0);
		cJSON_PrintPreallocated(j, fwd, sizeof(fwd), 0);
		cJSON_Delete(j);
		char rvd_resp[256];
		rss_ctrl_send_command("/var/run/rss/rvd.sock", fwd, rvd_resp, sizeof(rvd_resp),
				      1000);
	}
	return rss_ctrl_resp_ok(resp, resp_size);
}

static int rod_ctrl_handler(const char *cmd_json, char *resp_buf, int resp_buf_size, void *userdata)
{
	rod_state_t *st = userdata;

	int rc =
		rss_ctrl_handle_common(cmd_json, resp_buf, resp_buf_size, st->cfg, st->config_path);
	if (rc >= 0)
		return rc;

	char cmd[64];
	if (rss_json_get_str(cmd_json, "cmd", cmd, sizeof(cmd)) != 0)
		return rss_ctrl_resp_error(resp_buf, resp_buf_size, "missing cmd");

	if (strcmp(cmd, "set-text") == 0)
		return handle_set_text(st, cmd_json, resp_buf, resp_buf_size);

	if (strcmp(cmd, "set-font-color") == 0)
		return handle_color_change(st, cmd_json, resp_buf, resp_buf_size, "font_color",
					   &st->settings.font_color);

	if (strcmp(cmd, "set-stroke-color") == 0)
		return handle_color_change(st, cmd_json, resp_buf, resp_buf_size, "stroke_color",
					   &st->settings.stroke_color);

	if (strcmp(cmd, "set-stroke-size") == 0) {
		int val;
		if (rss_json_get_int(cmd_json, "value", &val) != 0 || val < 0 || val > 5)
			return rss_ctrl_resp_error(resp_buf, resp_buf_size, "need value 0-5");
		st->settings.font_stroke = val;
		rss_config_set_int(st->cfg, "osd", "font_stroke", val);
		mark_all_dirty(st);
		return rss_ctrl_resp_ok(resp_buf, resp_buf_size);
	}

	if (strcmp(cmd, "set-position") == 0)
		return handle_set_position(st, cmd_json, resp_buf, resp_buf_size);

	if (strcmp(cmd, "enable-time") == 0 || strcmp(cmd, "enable-uptime") == 0 ||
	    strcmp(cmd, "enable-text") == 0 || strcmp(cmd, "enable-logo") == 0)
		return handle_enable(st, cmd_json, resp_buf, resp_buf_size, cmd);

	if (strcmp(cmd, "set-font-size") == 0 || strcmp(cmd, "set-time-font-size") == 0 ||
	    strcmp(cmd, "set-uptime-font-size") == 0 || strcmp(cmd, "set-text-font-size") == 0)
		return handle_font_size_change(st, cmd_json, resp_buf, resp_buf_size, cmd);

	if (strcmp(cmd, "elements") == 0)
		return handle_elements_list(st, resp_buf, resp_buf_size);

	if (strcmp(cmd, "add-element") == 0)
		return handle_add_element(st, cmd_json, resp_buf, resp_buf_size);

	if (strcmp(cmd, "remove-element") == 0)
		return handle_remove_element(st, cmd_json, resp_buf, resp_buf_size);

	if (strcmp(cmd, "set-element") == 0)
		return handle_set_element(st, cmd_json, resp_buf, resp_buf_size);

	if (strcmp(cmd, "set-var") == 0)
		return handle_set_var(st, cmd_json, resp_buf, resp_buf_size);

	if (strcmp(cmd, "show-element") == 0)
		return handle_show_hide(st, cmd_json, resp_buf, resp_buf_size, true);

	if (strcmp(cmd, "hide-element") == 0)
		return handle_show_hide(st, cmd_json, resp_buf, resp_buf_size, false);

	if (strcmp(cmd, "config-show") == 0) {
		char fc[16], sc[16];
		snprintf(fc, sizeof(fc), "0x%08X", st->settings.font_color);
		snprintf(sc, sizeof(sc), "0x%08X", st->settings.stroke_color);
		cJSON *r = cJSON_CreateObject();
		if (!r)
			return rss_ctrl_resp_error(resp_buf, resp_buf_size, "alloc");
		cJSON_AddStringToObject(r, "status", "ok");
		cJSON *cfg = cJSON_AddObjectToObject(r, "config");
		cJSON_AddNumberToObject(cfg, "font_size", st->settings.font_size);
		cJSON_AddStringToObject(cfg, "font_color", fc);
		cJSON_AddStringToObject(cfg, "stroke_color", sc);
		cJSON_AddNumberToObject(cfg, "font_stroke", st->settings.font_stroke);
		cJSON_AddNumberToObject(cfg, "elements", st->elem_count);
		return rss_ctrl_resp_json(resp_buf, resp_buf_size, r);
	}

	/* Default: status */
	int region_count = 0;
	for (int i = 0; i < st->elem_count; i++) {
		if (!st->elements[i].active)
			continue;
		for (int s = 0; s < st->stream_count; s++) {
			if (st->elements[i].streams[s].shm)
				region_count++;
		}
	}

	cJSON *r = cJSON_CreateObject();
	if (!r)
		return rss_ctrl_resp_error(resp_buf, resp_buf_size, "alloc");
	cJSON_AddStringToObject(r, "status", "ok");
	cJSON_AddNumberToObject(r, "streams", st->stream_count);
	cJSON_AddNumberToObject(r, "elements", st->elem_count);
	cJSON_AddNumberToObject(r, "regions", region_count);
	return rss_ctrl_resp_json(resp_buf, resp_buf_size, r);
}

/* ── Entry point ── */

int main(int argc, char **argv)
{
	rss_daemon_ctx_t ctx;
	int ret = rss_daemon_init(&ctx, "rod", argc, argv, NULL);
	if (ret != 0)
		return ret < 0 ? 1 : 0;
	if (!rss_config_get_bool(ctx.cfg, "osd", "enabled", true)) {
		RSS_INFO("OSD disabled in config, exiting");
		rss_config_free(ctx.cfg);
		rss_daemon_cleanup("rod");
		return 0;
	}

	rod_state_t st = {0};
	st.cfg = ctx.cfg;
	st.config_path = ctx.config_path;
	st.running = ctx.running;
	load_config(&st);
	init_elements_from_config(&st);

	/* Init fonts for all text elements */
	for (int i = 0; i < st.elem_count; i++) {
		rod_element_t *e = &st.elements[i];
		if (!e->active || e->type != ROD_ELEM_TEXT)
			continue;

		for (int s = 0; s < st.stream_count; s++) {
			int fs = e->font_size > 0 ? e->font_size : st.settings.font_size;
			if (s > 0 && st.stream_h[0] > 0) {
				fs = fs * st.stream_h[s] / st.stream_h[0];
				if (fs < 12)
					fs = 12;
			}
			int fi = rod_alloc_font(&st, s, fs);
			if (fi < 0) {
				RSS_FATAL("font init failed for stream %d element %s", s, e->name);
				goto cleanup;
			}
			e->font_idx = fi;
		}
	}

	/* Load logo images */
	for (int i = 0; i < st.elem_count; i++) {
		rod_element_t *e = &st.elements[i];
		if (!e->active || e->type != ROD_ELEM_IMAGE)
			continue;

		if (e->image_path[0] && e->image_w > 0 && e->image_h > 0) {
			if (rod_load_logo(e->image_path, e->image_w, e->image_h, &e->image_data) ==
			    0) {
				RSS_DEBUG("logo loaded: %s (%dx%d)", e->image_path, e->image_w,
					  e->image_h);
			}
		}

		/* Sub-stream logo variant */
		if (st.stream_count > 1) {
			e->image_sub_w = 100;
			e->image_sub_h = 30;
			if (rod_load_logo("/usr/share/images/thingino_100x30.bgra", e->image_sub_w,
					  e->image_sub_h, &e->image_sub_data) != 0) {
				e->image_sub_w = 0;
				e->image_sub_h = 0;
			}
		}
	}

	create_all_shms(&st);

	/* Push all element positions to RVD so it uses the correct
	 * placement when discovering SHM regions via /dev/shm scan. */
	for (int i = 0; i < st.elem_count; i++) {
		rod_element_t *e = &st.elements[i];
		if (!e->active || !e->position[0])
			continue;
		for (int s = 0; s < st.stream_count; s++) {
			char fwd[128];
			cJSON *j = cJSON_CreateObject();
			if (!j)
				continue;
			cJSON_AddStringToObject(j, "cmd", "osd-position");
			cJSON_AddNumberToObject(j, "stream", s);
			cJSON_AddStringToObject(j, "region", e->name);
			cJSON_AddStringToObject(j, "pos", e->position);
			cJSON_PrintPreallocated(j, fwd, sizeof(fwd), 0);
			cJSON_Delete(j);
			char rvd_resp[256];
			rss_ctrl_send_command("/var/run/rss/rvd.sock", fwd, rvd_resp,
					      sizeof(rvd_resp), 1000);
		}
	}

	rss_mkdir_p("/var/run/rss");
	st.ctrl = rss_ctrl_listen("/var/run/rss/rod.sock");

	int epoll_fd = -1;
	int ctrl_fd = st.ctrl ? rss_ctrl_get_fd(st.ctrl) : -1;
	if (ctrl_fd >= 0) {
		epoll_fd = epoll_create1(0);
		if (epoll_fd >= 0) {
			struct epoll_event ev = {.events = EPOLLIN, .data.fd = ctrl_fd};
			if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, ctrl_fd, &ev) < 0)
				RSS_ERROR("epoll_ctl add ctrl_fd: %s", strerror(errno));
		}
	}

	render_tick(&st);

	int64_t last_tick = rss_timestamp_us();
	int64_t last_detect = 0;

	while (*st.running) {
		int64_t now = rss_timestamp_us();
		if (now - last_tick >= 1000000) {
			render_tick(&st);
			last_tick = now;
		}

		if (st.detect_enabled && now - last_detect >= 500000) {
			rod_element_t *det = rod_find_element(&st, "detect");
			if (det && det->active && det->visible)
				render_detections(&st, det);
			last_detect = now;
		}

		if (epoll_fd >= 0) {
			struct epoll_event ev;
			int n = epoll_wait(epoll_fd, &ev, 1, 100);
			if (n > 0 && st.ctrl)
				rss_ctrl_accept_and_handle(st.ctrl, rod_ctrl_handler, &st);
		} else {
			usleep(100000);
		}
	}

	RSS_INFO("rod shutting down");

	if (epoll_fd >= 0)
		close(epoll_fd);
	if (st.ctrl)
		rss_ctrl_destroy(st.ctrl);

	for (int i = 0; i < st.elem_count; i++) {
		rod_element_t *e = &st.elements[i];
		if (!e->active)
			continue;
		for (int s = 0; s < st.stream_count; s++) {
			if (e->streams[s].shm)
				rss_osd_destroy(e->streams[s].shm);
		}
		free(e->image_data);
		free(e->image_sub_data);
	}

cleanup:
	for (int s = 0; s < st.stream_count; s++) {
		for (int fi = 0; fi < ROD_MAX_FONTS; fi++) {
			if (st.fonts[s][fi].refcount > 0)
				rod_render_deinit(&st, s, fi);
		}
	}

	rss_config_free(ctx.cfg);
	rss_daemon_cleanup("rod");

	return 0;
}

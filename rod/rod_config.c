/*
 * rod_config.c -- Config loading and OSD element initialization
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rod.h"

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

void load_config(rod_state_t *st)
{
	rss_config_t *cfg = st->cfg;
	rod_config_t *c = &st->settings;

	c->enabled = rss_config_get_bool(cfg, "osd", "enabled", true);
	rss_strlcpy(c->font_path,
		    rss_config_get_str(cfg, "osd", "font", "/usr/share/fonts/default.ttf"),
		    sizeof(c->font_path));
	c->font_size = rss_config_get_int(cfg, "osd", "font_size", 24);
	c->font_color = parse_color(rss_config_get_str(cfg, "osd", "font_color", "0xFFFFFFFF"));
	c->stroke_color = parse_color(rss_config_get_str(cfg, "osd", "stroke_color", "0xFF000000"));
	c->font_stroke = rss_config_get_int(cfg, "osd", "font_stroke", 1);
	rss_strlcpy(c->time_format,
		    rss_config_get_str(cfg, "osd", "time_format", "%Y-%m-%d %H:%M:%S"),
		    sizeof(c->time_format));

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
	st->hostname[sizeof(st->hostname) - 1] = '\0';
}

/* ── [osd.*] section parser ── */

struct osd_section_ctx {
	rod_state_t *st;
	int count;
};

static void load_osd_section(const char *section, void *userdata)
{
	struct osd_section_ctx *ctx = userdata;
	rod_state_t *st = ctx->st;
	rss_config_t *cfg = st->cfg;

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
	else if (strcmp(type_str, "receipt") == 0)
		type = ROD_ELEM_RECEIPT;

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

	if (type == ROD_ELEM_RECEIPT) {
		e->receipt.max_lines = rss_config_get_int(cfg, section, "max_lines", 20);
		if (e->receipt.max_lines > ROD_RECEIPT_MAX_LINES)
			e->receipt.max_lines = ROD_RECEIPT_MAX_LINES;
		e->receipt.max_line_len = rss_config_get_int(cfg, section, "max_line_length", 80);
		if (e->receipt.max_line_len > ROD_RECEIPT_MAX_LINE)
			e->receipt.max_line_len = ROD_RECEIPT_MAX_LINE;
		e->receipt.accum_timeout = rss_config_get_int(cfg, section, "timeout", 0);
		e->receipt.bg_color =
			parse_color(rss_config_get_str(cfg, section, "bg_color", "0x80000000"));
		e->receipt.input_fd = -1;
		const char *source = rss_config_get_str(cfg, section, "source", "");
		if (strcmp(source, "fifo") == 0) {
			const char *dev = rss_config_get_str(cfg, section, "device", "");
			if (dev[0]) {
				rss_strlcpy(e->receipt.input_path, dev,
					    sizeof(e->receipt.input_path));
				mkfifo(dev, 0666);
				int fd = open(dev, O_RDONLY | O_NONBLOCK);
				if (fd >= 0) {
					e->receipt.input_fd = fd;
					RSS_INFO("receipt '%s': opened FIFO %s", name, dev);
				} else {
					RSS_WARN("receipt '%s': failed to open FIFO %s: %s", name,
						 dev, strerror(errno));
				}
			}
		} else if (strcmp(source, "uart") == 0) {
			const char *dev = rss_config_get_str(cfg, section, "device", "");
			if (dev[0]) {
				rss_strlcpy(e->receipt.input_path, dev,
					    sizeof(e->receipt.input_path));
				int fd = open(dev, O_RDONLY | O_NOCTTY | O_NONBLOCK);
				if (fd >= 0) {
					e->receipt.input_fd = fd;
					RSS_INFO("receipt '%s': opened UART %s", name, dev);
				} else {
					RSS_WARN("receipt '%s': failed to open %s: %s", name, dev,
						 strerror(errno));
				}
			}
		}
	}

	int stroke = rss_config_get_int(cfg, section, "stroke_size", -1);
	if (stroke >= 0)
		e->stroke_size = stroke;

	const char *color_str = rss_config_get_str(cfg, section, "color", NULL);
	if (color_str) {
		e->color = (uint32_t)strtoul(color_str, NULL, 0);
		e->has_color = true;
	}

	const char *sc_str = rss_config_get_str(cfg, section, "stroke_color", NULL);
	if (sc_str) {
		e->stroke_color = (uint32_t)strtoul(sc_str, NULL, 0);
		e->has_stroke_color = true;
	}

	ctx->count++;
	RSS_DEBUG("osd element from config: [%s] name=%s type=%s", section, name, type_str);
}

void init_elements_from_config(rod_state_t *st)
{
	struct osd_section_ctx ctx = {.st = st, .count = 0};
	rss_config_foreach_section(st->cfg, "osd.", load_osd_section, &ctx);

	if (ctx.count == 0)
		RSS_WARN("no [osd.*] sections found in config -- OSD will be empty");
	else
		RSS_INFO("loaded %d OSD elements from config", ctx.count);

	if (!rod_find_element(st, "privacy")) {
		rod_add_element(st, "privacy", ROD_ELEM_TEXT, "Privacy Mode", "center", 1, 0, 20,
				ROD_UPDATE_CHANGE);
		rod_element_t *e = rod_find_element(st, "privacy");
		if (e)
			e->visible = false;
	}

	if (st->detect_enabled && !rod_find_element(st, "detect")) {
		rod_add_element(st, "detect", ROD_ELEM_OVERLAY, NULL, "0,0", 0, 0, 0,
				ROD_UPDATE_TICK);
		rod_element_t *e = rod_find_element(st, "detect");
		if (e)
			e->sub_streams_only = true;
	}
}

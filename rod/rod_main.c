/*
 * rod_main.c -- Raptor OSD Daemon
 *
 * Renders text overlays (timestamp, uptime, camera name) and logos
 * into BGRA bitmaps, published to OSD SHM double-buffers for RVD.
 *
 * ROD has no HAL dependency -- it is purely a bitmap producer.
 * RVD handles OSD group/region creation and hardware updates.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/epoll.h>

#include "rod.h"

static const char *region_names[] = {"time", "uptime", "text", "logo", "privacy", "detect"};

/* Parse 0xAARRGGBB hex color string */
static uint32_t parse_color(const char *s)
{
	if (!s)
		return 0xFFFFFFFF;
	return (uint32_t)strtoul(s, NULL, 0);
}

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

	/* Stream dimensions — sensor 0 */
	st->stream_w[0] = rss_config_get_int(cfg, "stream0", "width", 1920);
	st->stream_h[0] = rss_config_get_int(cfg, "stream0", "height", 1080);
	st->stream_count = 1;

	if (rss_config_get_bool(cfg, "stream1", "enabled", true)) {
		st->stream_w[1] = rss_config_get_int(cfg, "stream1", "width", 640);
		st->stream_h[1] = rss_config_get_int(cfg, "stream1", "height", 360);
		st->stream_count = 2;
	}

	/* Multi-sensor: sensor 1 streams (indices 2,3) and sensor 2 (indices 4,5) */
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

	/* Detection overlay — enabled when motion detection is on */
	st->detect_enabled = rss_config_get_bool(cfg, "motion", "enabled", false);
}

static void create_region_shm(rod_state_t *st, int s, int role, uint32_t w, uint32_t h)
{
	char name[64];
	snprintf(name, sizeof(name), "osd_%d_%s", s, region_names[role]);

	/* Ensure even dimensions */
	w = (w + 1) & ~1;
	h = (h + 1) & ~1;

	rss_osd_shm_t *shm = rss_osd_create(name, w, h);
	if (!shm) {
		RSS_WARN("failed to create OSD SHM: %s", name);
		return;
	}

	st->regions[s][role].shm = shm;
	st->regions[s][role].width = w;
	st->regions[s][role].height = h;
	st->regions[s][role].role = role;
	st->regions[s][role].enabled = true;
	st->regions[s][role].needs_update = true;

	RSS_INFO("osd shm %s: %ux%u", name, w, h);
}

/* Helper: get SHM dimensions for a text region from its font context */
static void font_region_dims(rod_font_t *f, int chars, int stroke, uint32_t *w, uint32_t *h)
{
	int adv = f->max_text_width / 24;
	if (adv < 10)
		adv = 10;
	int pad = stroke > 0 ? stroke * 2 : 0;
	*w = chars * adv + pad;
	*h = f->text_height;
}

static void create_shms(rod_state_t *st)
{
	for (int s = 0; s < st->stream_count; s++) {
		uint32_t w, h;

		if (st->settings.time_enabled) {
			font_region_dims(&st->fonts[s][ROD_FONT_TIME], ROD_TIME_CHARS,
					 st->settings.font_stroke, &w, &h);
			create_region_shm(st, s, ROD_REGION_TIME, w, h);
		}

		if (st->settings.uptime_enabled) {
			font_region_dims(&st->fonts[s][ROD_FONT_UPTIME], ROD_UPTIME_CHARS,
					 st->settings.font_stroke, &w, &h);
			create_region_shm(st, s, ROD_REGION_UPTIME, w, h);
		}

		if (st->settings.text_enabled) {
			font_region_dims(&st->fonts[s][ROD_FONT_TEXT], ROD_TEXT_CHARS,
					 st->settings.font_stroke, &w, &h);
			create_region_shm(st, s, ROD_REGION_TEXT, w, h);
		}

		/* Privacy uses time's font */
		font_region_dims(&st->fonts[s][ROD_FONT_TIME], ROD_TIME_CHARS,
				 st->settings.font_stroke, &w, &h);
		create_region_shm(st, s, ROD_REGION_PRIVACY, w, h);

		if (st->settings.logo_enabled) {
			int lw, lh;
			if (s == 0) {
				lw = st->settings.logo_width;
				lh = st->settings.logo_height;
			} else {
				/* Scale logo for sub stream */
				lw = st->logo_sub_w;
				lh = st->logo_sub_h;
			}
			if (lw > 0 && lh > 0)
				create_region_shm(st, s, ROD_REGION_LOGO, lw, lh);
		}

		/* Detection bounding box overlay — sub-stream only.
		 * Full sub-stream size so box coordinates map 1:1. */
		if (s > 0 && st->detect_enabled)
			create_region_shm(st, s, ROD_REGION_DETECT, st->stream_w[s],
					  st->stream_h[s]);
	}
}

static void render_logo(rod_state_t *st, int s)
{
	rod_region_t *reg = &st->regions[s][ROD_REGION_LOGO];
	if (!reg->enabled || !reg->shm)
		return;

	uint8_t *logo = (s == 0) ? st->logo_data : st->logo_sub_data;
	int logo_w = (s == 0) ? st->settings.logo_width : st->logo_sub_w;
	int logo_h = (s == 0) ? st->settings.logo_height : st->logo_sub_h;
	if (!logo)
		return;

	uint8_t *buf = rss_osd_get_draw_buffer(reg->shm);
	if (!buf)
		return;

	/* Clear then row-copy (region may be wider than logo due to alignment) */
	memset(buf, 0, reg->width * reg->height * 4);
	for (int row = 0; row < logo_h && row < (int)reg->height; row++)
		memcpy(buf + row * reg->width * 4, logo + row * logo_w * 4, logo_w * 4);

	rss_osd_publish(reg->shm);
	reg->needs_update = false;
}

/* Per-role text alignment: time=left, uptime=right, text=center */
static int role_align(int role)
{
	switch (role) {
	case ROD_REGION_UPTIME:
		return 2; /* right */
	case ROD_REGION_TEXT:
	case ROD_REGION_PRIVACY:
		return 1; /* center */
	default:
		return 0; /* left */
	}
}

static void render_text_region(rod_state_t *st, int s, int role, int font_idx, const char *text)
{
	rod_region_t *reg = &st->regions[s][role];
	if (!reg->enabled || !reg->shm)
		return;

	uint8_t *buf = rss_osd_get_draw_buffer(reg->shm);
	if (!buf)
		return;

	rod_draw_text(st, s, font_idx, buf, reg->width, reg->height, text, role_align(role));
	rss_osd_publish(reg->shm);
}

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

/* ── Detection bounding box overlay ── */

static void render_detections(rod_state_t *st)
{
	if (!st->detect_enabled)
		return;

	/* Query RVD for current detections */
	char resp[2048];
	int ret = rss_ctrl_send_command("/var/run/rss/rvd.sock", "{\"cmd\":\"ivs-detections\"}",
					resp, sizeof(resp), 500);
	if (ret < 0)
		return;

	/* Parse count */
	int count = 0;
	rss_json_get_int(resp, "count", &count);
	if (count < 0)
		count = 0;
	if (count > 20)
		count = 20;

	/* Render on sub-streams only */
	for (int s = 1; s < st->stream_count; s++) {
		rod_region_t *reg = &st->regions[s][ROD_REGION_DETECT];
		if (!reg->enabled || !reg->shm)
			continue;

		uint8_t *buf = rss_osd_get_draw_buffer(reg->shm);
		if (!buf)
			continue;

		/* Clear to fully transparent */
		memset(buf, 0, reg->width * reg->height * 4);

		if (count > 0) {
			/* Parse detection array from JSON response.
			 * Format: {"detections":[{"x0":N,"y0":N,"x1":N,"y1":N,...},...]} */
			const char *p = strstr(resp, "\"detections\"");
			if (!p)
				goto publish;

			p = strchr(p, '[');
			if (!p)
				goto publish;
			p++; /* skip '[' */

			for (int i = 0; i < count; i++) {
				const char *obj = strchr(p, '{');
				if (!obj)
					break;
				const char *end = strchr(obj, '}');
				if (!end)
					break;

				/* Parse coords within this object only */
				int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
				const char *k;
				k = strstr(obj, "\"x0\":");
				if (k && k < end)
					sscanf(k + 5, "%d", &x0);
				k = strstr(obj, "\"y0\":");
				if (k && k < end)
					sscanf(k + 5, "%d", &y0);
				k = strstr(obj, "\"x1\":");
				if (k && k < end)
					sscanf(k + 5, "%d", &x1);
				k = strstr(obj, "\"y1\":");
				if (k && k < end)
					sscanf(k + 5, "%d", &y1);

				rod_draw_rect_outline(buf, reg->width, reg->height, x0, y0, x1, y1,
						      0xFF00FF00, 2);
				p = end + 1;
			}
		}
	publish:
		rss_osd_publish(reg->shm);
	}
}

static void render_tick(rod_state_t *st)
{
	/* Format timestamp */
	char time_str[64];
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(time_str, sizeof(time_str), st->settings.time_format, &tm);

	/* Format uptime */
	char uptime_str[64];
	format_uptime(uptime_str, sizeof(uptime_str));

	for (int s = 0; s < st->stream_count; s++) {
		/* Time (1Hz) */
		if (st->settings.time_enabled)
			render_text_region(st, s, ROD_REGION_TIME, ROD_FONT_TIME, time_str);

		/* Uptime (1Hz) */
		if (st->settings.uptime_enabled)
			render_text_region(st, s, ROD_REGION_UPTIME, ROD_FONT_UPTIME, uptime_str);

		/* Text (on change only) */
		if (st->settings.text_enabled && st->regions[s][ROD_REGION_TEXT].needs_update) {
			render_text_region(st, s, ROD_REGION_TEXT, ROD_FONT_TEXT,
					   st->settings.text_string);
			st->regions[s][ROD_REGION_TEXT].needs_update = false;
		}

		/* Privacy text (once) */
		if (st->regions[s][ROD_REGION_PRIVACY].needs_update) {
			render_text_region(st, s, ROD_REGION_PRIVACY, ROD_FONT_TIME,
					   "Privacy Mode");
			st->regions[s][ROD_REGION_PRIVACY].needs_update = false;
		}

		/* Logo (once) */
		if (st->settings.logo_enabled && st->regions[s][ROD_REGION_LOGO].needs_update)
			render_logo(st, s);

		/* Heartbeat: touch any active region so RVD knows we're alive.
		 * Needed when time/uptime are disabled and only static regions remain. */
		for (int r = 0; r < ROD_MAX_REGIONS; r++) {
			rod_region_t *reg = &st->regions[s][r];
			if (reg->enabled && reg->shm) {
				rss_osd_heartbeat(reg->shm);
				break; /* one per stream is enough */
			}
		}
	}
}

/* ── Control socket ── */

static int rod_ctrl_handler(const char *cmd_json, char *resp_buf, int resp_buf_size, void *userdata)
{
	rod_state_t *st = userdata;

	int rc =
		rss_ctrl_handle_common(cmd_json, resp_buf, resp_buf_size, st->cfg, st->config_path);
	if (rc >= 0)
		return rc;

	if (strstr(cmd_json, "\"set-text\"")) {
		char val[128];
		if (rss_json_get_str(cmd_json, "value", val, sizeof(val)) == 0) {
			/* Sanitize: strip control characters */
			for (int i = 0; val[i]; i++) {
				if ((unsigned char)val[i] < 0x20)
					val[i] = ' ';
			}
			rss_strlcpy(st->settings.text_string, val,
				    sizeof(st->settings.text_string));
			rss_config_set_str(st->cfg, "osd", "text_string", val);
			for (int s = 0; s < st->stream_count; s++)
				st->regions[s][ROD_REGION_TEXT].needs_update = true;
			return rss_ctrl_resp_ok(resp_buf, resp_buf_size);
		} else {
			return rss_ctrl_resp_error(resp_buf, resp_buf_size, "missing value");
		}
	}

	if (strstr(cmd_json, "\"set-font-color\"")) {
		char val[16];
		if (rss_json_get_str(cmd_json, "value", val, sizeof(val)) == 0) {
			st->settings.font_color = parse_color(val);
			rss_config_set_str(st->cfg, "osd", "font_color", val);
			for (int s = 0; s < st->stream_count; s++)
				for (int r = 0; r < ROD_MAX_REGIONS; r++)
					if (st->regions[s][r].enabled)
						st->regions[s][r].needs_update = true;
			return rss_ctrl_resp_ok(resp_buf, resp_buf_size);
		} else {
			return rss_ctrl_resp_error(resp_buf, resp_buf_size, "missing value");
		}
	}

	if (strstr(cmd_json, "\"set-stroke-color\"")) {
		char val[16];
		if (rss_json_get_str(cmd_json, "value", val, sizeof(val)) == 0) {
			st->settings.stroke_color = parse_color(val);
			rss_config_set_str(st->cfg, "osd", "stroke_color", val);
			for (int s = 0; s < st->stream_count; s++)
				for (int r = 0; r < ROD_MAX_REGIONS; r++)
					if (st->regions[s][r].enabled)
						st->regions[s][r].needs_update = true;
			return rss_ctrl_resp_ok(resp_buf, resp_buf_size);
		} else {
			return rss_ctrl_resp_error(resp_buf, resp_buf_size, "missing value");
		}
	}

	if (strstr(cmd_json, "\"set-stroke-size\"")) {
		int val;
		if (rss_json_get_int(cmd_json, "value", &val) == 0 && val >= 0 && val <= 5) {
			st->settings.font_stroke = val;
			rss_config_set_int(st->cfg, "osd", "font_stroke", val);
			for (int s = 0; s < st->stream_count; s++)
				for (int r = 0; r < ROD_MAX_REGIONS; r++)
					if (st->regions[s][r].enabled)
						st->regions[s][r].needs_update = true;
			return rss_ctrl_resp_ok(resp_buf, resp_buf_size);
		} else {
			return rss_ctrl_resp_error(resp_buf, resp_buf_size, "need value 0-5");
		}
	}

	/* ── Element positioning — lightweight, no pipeline restart ── */
	if (strstr(cmd_json, "\"set-position\"")) {
		char element[16] = "", pos[32] = "";
		rss_json_get_str(cmd_json, "element", element, sizeof(element));
		rss_json_get_str(cmd_json, "pos", pos, sizeof(pos));

		if (!element[0] || !pos[0])
			return rss_ctrl_resp_error(resp_buf, resp_buf_size, "need element and pos");

		/* Forward to RVD — optional stream filter (-1 = all) */
		int target_stream = -1;
		rss_json_get_int(cmd_json, "stream", &target_stream);
		for (int s = 0; s < st->stream_count; s++) {
			if (target_stream >= 0 && s != target_stream)
				continue;
			char cmd[128];
			snprintf(cmd, sizeof(cmd),
				 "{\"cmd\":\"osd-position\",\"stream\":%d,"
				 "\"region\":\"%s\",\"pos\":\"%s\"}",
				 s, element, pos);
			char rvd_resp[256];
			rss_ctrl_send_command("/var/run/rss/rvd.sock", cmd, rvd_resp,
					      sizeof(rvd_resp), 1000);
		}

		RSS_INFO("set-position: %s → %s", element, pos);
		return rss_ctrl_resp_ok(resp_buf, resp_buf_size);
	}

	/* ── Element show/hide — lightweight, no pipeline restart ── */
	if (strstr(cmd_json, "\"enable-time\"") || strstr(cmd_json, "\"enable-uptime\"") ||
	    strstr(cmd_json, "\"enable-text\"") || strstr(cmd_json, "\"enable-logo\"")) {
		int val;
		if (rss_json_get_int(cmd_json, "value", &val) != 0)
			return rss_ctrl_resp_error(resp_buf, resp_buf_size, "need value (0/1)");

		bool enable = !!val;
		const char *region_name;
		bool *setting;
		const char *key;

		if (strstr(cmd_json, "\"enable-time\"")) {
			region_name = "time";
			setting = &st->settings.time_enabled;
			key = "time_enabled";
		} else if (strstr(cmd_json, "\"enable-uptime\"")) {
			region_name = "uptime";
			setting = &st->settings.uptime_enabled;
			key = "uptime_enabled";
		} else if (strstr(cmd_json, "\"enable-logo\"")) {
			region_name = "logo";
			setting = &st->settings.logo_enabled;
			key = "logo_enabled";
		} else {
			region_name = "text";
			setting = &st->settings.text_enabled;
			key = "text_enabled";
		}

		/* Optional stream filter (-1 = all streams, persists to config) */
		int target_stream = -1;
		rss_json_get_int(cmd_json, "stream", &target_stream);

		if (target_stream < 0) {
			/* Global: update config + ROD state */
			if (*setting == enable)
				return rss_ctrl_resp_ok(resp_buf, resp_buf_size);
			*setting = enable;
			rss_config_set_str(st->cfg, "osd", key, enable ? "true" : "false");
		}

		/* Tell RVD to show/hide the HAL region */
		for (int s = 0; s < st->stream_count; s++) {
			if (target_stream >= 0 && s != target_stream)
				continue;
			char cmd[128];
			snprintf(cmd, sizeof(cmd),
				 "{\"cmd\":\"osd-show\",\"stream\":%d,\"region\":\"%s\","
				 "\"show\":%d}",
				 s, region_name, enable ? 1 : 0);
			char rvd_resp[256];
			rss_ctrl_send_command("/var/run/rss/rvd.sock", cmd, rvd_resp,
					      sizeof(rvd_resp), 1000);
		}

		RSS_INFO("%s: %s", key, enable ? "on" : "off");
		return rss_ctrl_resp_ok(resp_buf, resp_buf_size);
	}

	/* Per-element font size — check before global (substring match) */
	if (strstr(cmd_json, "\"set-time-font-size\"") ||
	    strstr(cmd_json, "\"set-uptime-font-size\"") ||
	    strstr(cmd_json, "\"set-text-font-size\"")) {
		int val;
		if (rss_json_get_int(cmd_json, "value", &val) != 0 || val < 10 || val > 72)
			return rss_ctrl_resp_error(resp_buf, resp_buf_size, "need value 10-72");

		int fi;
		const char *key;
		int *setting;
		if (strstr(cmd_json, "\"set-time-font-size\"")) {
			fi = ROD_FONT_TIME;
			key = "time_font_size";
			setting = &st->settings.time_font_size;
		} else if (strstr(cmd_json, "\"set-uptime-font-size\"")) {
			fi = ROD_FONT_UPTIME;
			key = "uptime_font_size";
			setting = &st->settings.uptime_font_size;
		} else {
			fi = ROD_FONT_TEXT;
			key = "text_font_size";
			setting = &st->settings.text_font_size;
		}

		RSS_INFO("set-%s: → %d", key, val);

		/* Re-rasterize only this element's font */
		for (int s = 0; s < st->stream_count; s++) {
			int fs = val;
			if (s > 0) {
				fs = fs * st->stream_h[s] / st->stream_h[0];
				if (fs < 12)
					fs = 12;
			}
			rod_render_deinit(st, s, fi);
			if (rod_render_init(st, s, fi, fs) < 0)
				RSS_ERROR("font re-init failed s%d fi%d", s, fi);
		}

		/* Destroy and recreate only the affected SHM regions */
		static const int font_to_region[][2] = {
			{ROD_REGION_TIME, ROD_REGION_PRIVACY}, /* TIME font → time + privacy */
			{ROD_REGION_UPTIME, -1},	       /* UPTIME font → uptime only */
			{ROD_REGION_TEXT, -1},		       /* TEXT font → text only */
		};
		for (int s = 0; s < st->stream_count; s++) {
			for (int ri = 0; ri < 2 && font_to_region[fi][ri] >= 0; ri++) {
				int r = font_to_region[fi][ri];
				if (st->regions[s][r].shm) {
					rss_osd_destroy(st->regions[s][r].shm);
					st->regions[s][r].shm = NULL;
					st->regions[s][r].enabled = false;
				}
			}
			uint32_t w, h;
			for (int ri = 0; ri < 2 && font_to_region[fi][ri] >= 0; ri++) {
				int r = font_to_region[fi][ri];
				int chars = (r == ROD_REGION_UPTIME) ? ROD_UPTIME_CHARS
					    : (r == ROD_REGION_TEXT) ? ROD_TEXT_CHARS
								     : ROD_TIME_CHARS;
				font_region_dims(&st->fonts[s][fi], chars, st->settings.font_stroke,
						 &w, &h);
				create_region_shm(st, s, r, w, h);
			}
		}

		*setting = val;
		rss_config_set_int(st->cfg, "osd", key, val);

		/* Notify RVD */
		char cmd[96];
		snprintf(cmd, sizeof(cmd), "{\"cmd\":\"osd-restart\",\"pool_kb\":0}");
		char rvd_resp[256];
		rss_ctrl_send_command("/var/run/rss/rvd.sock", cmd, rvd_resp, sizeof(rvd_resp),
				      5000);

		return rss_ctrl_resp_ok(resp_buf, resp_buf_size);
	}

	if (strstr(cmd_json, "\"set-font-size\"")) {
		int val;
		if (rss_json_get_int(cmd_json, "value", &val) != 0 || val < 10 || val > 72)
			return rss_ctrl_resp_error(resp_buf, resp_buf_size, "need value 10-72");

		RSS_INFO("set-font-size: %d → %d", st->settings.font_size, val);

		/* Re-rasterize glyphs at new size per stream */
		for (int s = 0; s < st->stream_count; s++) {
			int fs = val;
			if (s > 0) {
				fs = fs * st->stream_h[s] / st->stream_h[0];
				if (fs < 12)
					fs = 12;
			}
			for (int fi = 0; fi < ROD_FONT_COUNT; fi++) {
				rod_render_deinit(st, s, fi);
				if (rod_render_init(st, s, fi, fs) < 0) {
					RSS_ERROR("font re-init failed s%d fi%d", s, fi);
					return rss_ctrl_resp_error(resp_buf, resp_buf_size,
								   "font init failed");
				}
			}
		}

		/* Destroy text SHM regions (inode change triggers RVD reopen) */
		for (int s = 0; s < st->stream_count; s++) {
			for (int r = 0; r < ROD_MAX_REGIONS; r++) {
				if (r == ROD_REGION_LOGO || r == ROD_REGION_DETECT)
					continue; /* not font-dependent */
				if (st->regions[s][r].shm) {
					rss_osd_destroy(st->regions[s][r].shm);
					st->regions[s][r].shm = NULL;
					st->regions[s][r].enabled = false;
				}
			}
		}

		/* Recreate text SHM regions with new font dimensions */
		for (int s = 0; s < st->stream_count; s++) {
			rod_font_t *f = &st->fonts[s][ROD_FONT_TIME];
			int adv = f->max_text_width / 24;
			if (adv < 10)
				adv = 10;
			int pad = st->settings.font_stroke > 0 ? st->settings.font_stroke * 2 : 0;

			if (st->settings.time_enabled)
				create_region_shm(st, s, ROD_REGION_TIME,
						  ROD_TIME_CHARS * adv + pad, f->text_height);
			if (st->settings.uptime_enabled)
				create_region_shm(st, s, ROD_REGION_UPTIME,
						  ROD_UPTIME_CHARS * adv + pad, f->text_height);
			if (st->settings.text_enabled)
				create_region_shm(st, s, ROD_REGION_TEXT,
						  ROD_TEXT_CHARS * adv + pad, f->text_height);
			create_region_shm(st, s, ROD_REGION_PRIVACY, ROD_TIME_CHARS * adv + pad,
					  f->text_height);
		}

		/* Persist after success */
		st->settings.font_size = val;
		rss_config_set_int(st->cfg, "osd", "font_size", val);

		/* Tell RVD to restart OSD with resized pool.
		 * Calculate pool: worst case per-stream region bytes. */
		{
			rod_font_t *f0 = &st->fonts[0][ROD_FONT_TIME];
			int adv = f0->max_text_width / 24;
			if (adv < 10)
				adv = 10;
			int pad = st->settings.font_stroke > 0 ? st->settings.font_stroke * 2 : 0;
			uint32_t main_bytes = ((uint32_t)(ROD_TIME_CHARS + ROD_UPTIME_CHARS +
							  ROD_TEXT_CHARS + ROD_TIME_CHARS) *
						       adv +
					       pad * 4) *
					      f0->text_height * 4;
			main_bytes += 100 * 30 * 4;	 /* logo */
			uint32_t total = main_bytes * 2; /* main + sub estimate */
			total = total * 5 / 4;		 /* 25% headroom */
			uint32_t pool_kb = (total + 1023) / 1024;
			if (pool_kb < 448)
				pool_kb = 448; /* minimum */

			char cmd[96];
			snprintf(cmd, sizeof(cmd),
				 "{\"cmd\":\"osd-restart\",\"pool_kb\":%u,\"font_size\":%d}",
				 pool_kb, val);
			char rvd_resp[256];
			int ret = rss_ctrl_send_command("/var/run/rss/rvd.sock", cmd, rvd_resp,
							sizeof(rvd_resp), 5000);
			if (ret < 0)
				RSS_WARN("failed to notify RVD for OSD restart");
			else
				RSS_INFO("RVD osd-restart: pool_kb=%u resp=%s", pool_kb, rvd_resp);
		}

		return rss_ctrl_resp_ok(resp_buf, resp_buf_size);
	}

	if (strstr(cmd_json, "\"config-show\"")) {
		return rss_ctrl_resp(
			resp_buf, resp_buf_size,
			"{\"status\":\"ok\",\"config\":{"
			"\"font_size\":%d,"
			"\"font_color\":\"0x%08X\","
			"\"stroke_color\":\"0x%08X\","
			"\"font_stroke\":%d,"
			"\"time_enabled\":%s,"
			"\"uptime_enabled\":%s,"
			"\"text_enabled\":%s,"
			"\"text_string\":\"%s\","
			"\"logo_enabled\":%s}}",
			st->settings.font_size, st->settings.font_color, st->settings.stroke_color,
			st->settings.font_stroke, st->settings.time_enabled ? "true" : "false",
			st->settings.uptime_enabled ? "true" : "false",
			st->settings.text_enabled ? "true" : "false", st->settings.text_string,
			st->settings.logo_enabled ? "true" : "false");
	}

	/* Default: status */
	int region_count = 0;
	for (int s = 0; s < st->stream_count; s++)
		for (int r = 0; r < ROD_MAX_REGIONS; r++)
			if (st->regions[s][r].enabled)
				region_count++;

	return rss_ctrl_resp(resp_buf, resp_buf_size,
			     "{\"status\":\"ok\",\"streams\":%d,\"regions\":%d}", st->stream_count,
			     region_count);
}

/* ── Entry point ── */

int main(int argc, char **argv)
{
	rss_daemon_ctx_t ctx;
	int ret = rss_daemon_init(&ctx, "rod", argc, argv, NULL);
	if (ret != 0)
		return ret < 0 ? 1 : 0;
	/* Early exit if OSD disabled */
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

	/* Init fonts per stream per element */
	for (int s = 0; s < st.stream_count; s++) {
		/* Per-element sizes: use element override, fall back to global */
		int sizes[ROD_FONT_COUNT];
		sizes[ROD_FONT_TIME] = st.settings.time_font_size;
		sizes[ROD_FONT_UPTIME] = st.settings.uptime_font_size;
		sizes[ROD_FONT_TEXT] = st.settings.text_font_size;

		for (int fi = 0; fi < ROD_FONT_COUNT; fi++) {
			int fs = sizes[fi] > 0 ? sizes[fi] : st.settings.font_size;
			if (s > 0) {
				fs = fs * st.stream_h[s] / st.stream_h[0];
				if (fs < 12)
					fs = 12;
			}
			if (rod_render_init(&st, s, fi, fs) < 0) {
				RSS_FATAL("font init failed for stream %d element %d", s, fi);
				goto cleanup;
			}
		}
	}

	/* Load logo */
	if (st.settings.logo_enabled) {
		if (rod_load_logo(st.settings.logo_path, st.settings.logo_width,
				  st.settings.logo_height, &st.logo_data) == 0) {
			st.logo_data_size = st.settings.logo_width * st.settings.logo_height * 4;
			RSS_DEBUG("logo loaded: %s (%dx%d)", st.settings.logo_path,
				  st.settings.logo_width, st.settings.logo_height);
		}

		/* Sub-stream logo: try smaller variant */
		if (st.stream_count > 1) {
			/* Derive sub logo path: replace dimensions in filename */
			st.logo_sub_w = 100;
			st.logo_sub_h = 30;
			if (rod_load_logo("/usr/share/images/thingino_100x30.bgra", st.logo_sub_w,
					  st.logo_sub_h, &st.logo_sub_data) != 0) {
				/* No sub logo — disable for sub stream */
				st.logo_sub_w = 0;
				st.logo_sub_h = 0;
			}
		}
	}

	/* Create OSD SHMs */
	create_shms(&st);

	/* Control socket */
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

	/* Initial render of all regions */
	render_tick(&st);

	/* Main loop: 1Hz text render, 500ms detection overlay, 100ms ctrl poll */
	int64_t last_tick = rss_timestamp_us();
	int64_t last_detect = 0;

	while (*st.running) {
		int64_t now = rss_timestamp_us();
		if (now - last_tick >= 1000000) {
			render_tick(&st);
			last_tick = now;
		}

		if (st.detect_enabled && now - last_detect >= 500000) {
			render_detections(&st);
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

	/* Cleanup */
	if (epoll_fd >= 0)
		close(epoll_fd);
	if (st.ctrl)
		rss_ctrl_destroy(st.ctrl);

	for (int s = 0; s < st.stream_count; s++) {
		for (int r = 0; r < ROD_MAX_REGIONS; r++) {
			if (st.regions[s][r].shm)
				rss_osd_destroy(st.regions[s][r].shm);
		}
	}

	free(st.logo_data);
	free(st.logo_sub_data);

cleanup:
	for (int s = 0; s < st.stream_count; s++)
		for (int fi = 0; fi < ROD_FONT_COUNT; fi++)
			rod_render_deinit(&st, s, fi);

	rss_config_free(ctx.cfg);
	rss_daemon_cleanup("rod");

	return 0;
}

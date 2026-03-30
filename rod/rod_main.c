/*
 * rod_main.c -- Raptor OSD Daemon
 *
 * Renders text overlays (timestamp, uptime, camera name) and logos
 * into BGRA bitmaps, published to OSD SHM double-buffers for RVD.
 *
 * ROD has no HAL dependency -- it is purely a bitmap producer.
 * RVD handles OSD group/region creation and hardware updates.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/epoll.h>

#include "rod.h"

static const char *region_names[] = {"time", "uptime", "text", "logo", "privacy"};

/* Parse 0xAARRGGBB hex color string */
static uint32_t parse_color(const char *s)
{
	if (!s)
		return 0xFFFFFFFF;
	return (uint32_t)strtoul(s, NULL, 0);
}

static void load_config(rod_state_t *st)
{
	rss_config_t *cfg = st->config;
	rod_config_t *c = &st->cfg;

	c->enabled = rss_config_get_bool(cfg, "osd", "enabled", true);
	rss_strlcpy(c->font_path,
		    rss_config_get_str(cfg, "osd", "font", "/usr/share/fonts/default.ttf"),
		    sizeof(c->font_path));
	c->font_size = rss_config_get_int(cfg, "osd", "font_size", 24);
	c->font_color = parse_color(rss_config_get_str(cfg, "osd", "font_color", "0xFFFFFFFF"));
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

	/* Stream dimensions */
	st->stream_w[0] = rss_config_get_int(cfg, "stream0", "width", 1920);
	st->stream_h[0] = rss_config_get_int(cfg, "stream0", "height", 1080);
	st->stream_count = 1;

	if (rss_config_get_bool(cfg, "stream1", "enabled", true)) {
		st->stream_w[1] = rss_config_get_int(cfg, "stream1", "width", 640);
		st->stream_h[1] = rss_config_get_int(cfg, "stream1", "height", 360);
		st->stream_count = 2;
	}
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

static void create_shms(rod_state_t *st)
{
	for (int s = 0; s < st->stream_count; s++) {
		rod_font_t *f = &st->fonts[s];
		int adv = f->max_text_width / 24; /* approximate per-char advance */
		if (adv < 10)
			adv = 10;
		int pad = st->cfg.font_stroke > 0 ? st->cfg.font_stroke * 2 : 0;

		if (st->cfg.time_enabled)
			create_region_shm(st, s, ROD_REGION_TIME, ROD_TIME_CHARS * adv + pad,
					  f->text_height);

		if (st->cfg.uptime_enabled)
			create_region_shm(st, s, ROD_REGION_UPTIME, ROD_UPTIME_CHARS * adv + pad,
					  f->text_height);

		if (st->cfg.text_enabled)
			create_region_shm(st, s, ROD_REGION_TEXT, ROD_TEXT_CHARS * adv + pad,
					  f->text_height);

		/* Privacy text region — use full text width so centering
		 * works correctly within the RVD region */
		create_region_shm(st, s, ROD_REGION_PRIVACY, ROD_TIME_CHARS * adv + pad,
				  f->text_height);

		if (st->cfg.logo_enabled) {
			int lw, lh;
			if (s == 0) {
				lw = st->cfg.logo_width;
				lh = st->cfg.logo_height;
			} else {
				/* Scale logo for sub stream */
				lw = st->logo_sub_w;
				lh = st->logo_sub_h;
			}
			if (lw > 0 && lh > 0)
				create_region_shm(st, s, ROD_REGION_LOGO, lw, lh);
		}
	}
}

static void render_logo(rod_state_t *st, int s)
{
	rod_region_t *reg = &st->regions[s][ROD_REGION_LOGO];
	if (!reg->enabled || !reg->shm)
		return;

	uint8_t *logo = (s == 0) ? st->logo_data : st->logo_sub_data;
	int logo_w = (s == 0) ? st->cfg.logo_width : st->logo_sub_w;
	int logo_h = (s == 0) ? st->cfg.logo_height : st->logo_sub_h;
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

static void render_text_region(rod_state_t *st, int s, int role, const char *text)
{
	rod_region_t *reg = &st->regions[s][role];
	if (!reg->enabled || !reg->shm)
		return;

	uint8_t *buf = rss_osd_get_draw_buffer(reg->shm);
	if (!buf)
		return;

	rod_draw_text(st, s, buf, reg->width, reg->height, text, role_align(role));
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

static void render_tick(rod_state_t *st)
{
	/* Format timestamp */
	char time_str[64];
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(time_str, sizeof(time_str), st->cfg.time_format, &tm);

	/* Format uptime */
	char uptime_str[64];
	format_uptime(uptime_str, sizeof(uptime_str));

	for (int s = 0; s < st->stream_count; s++) {
		/* Time (1Hz) */
		if (st->cfg.time_enabled)
			render_text_region(st, s, ROD_REGION_TIME, time_str);

		/* Uptime (1Hz) */
		if (st->cfg.uptime_enabled)
			render_text_region(st, s, ROD_REGION_UPTIME, uptime_str);

		/* Text (on change only) */
		if (st->cfg.text_enabled && st->regions[s][ROD_REGION_TEXT].needs_update) {
			render_text_region(st, s, ROD_REGION_TEXT, st->cfg.text_string);
			st->regions[s][ROD_REGION_TEXT].needs_update = false;
		}

		/* Privacy text (once) */
		if (st->regions[s][ROD_REGION_PRIVACY].needs_update) {
			render_text_region(st, s, ROD_REGION_PRIVACY, "Privacy Mode");
			st->regions[s][ROD_REGION_PRIVACY].needs_update = false;
		}

		/* Logo (once) */
		if (st->cfg.logo_enabled && st->regions[s][ROD_REGION_LOGO].needs_update)
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

	int rc = rss_ctrl_handle_common(cmd_json, resp_buf, resp_buf_size, st->config, st->config_path);
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
			rss_strlcpy(st->cfg.text_string, val, sizeof(st->cfg.text_string));
			rss_config_set_str(st->config, "osd", "text_string", val);
			for (int s = 0; s < st->stream_count; s++)
				st->regions[s][ROD_REGION_TEXT].needs_update = true;
			snprintf(resp_buf, resp_buf_size, "{\"status\":\"ok\"}");
		} else {
			snprintf(resp_buf, resp_buf_size,
				 "{\"status\":\"error\",\"msg\":\"missing value\"}");
		}
		return (int)strlen(resp_buf);
	}

	if (strstr(cmd_json, "\"config-show\"")) {
		snprintf(resp_buf, resp_buf_size,
			 "{\"status\":\"ok\",\"config\":{"
			 "\"font_size\":%d,"
			 "\"time_enabled\":%s,"
			 "\"uptime_enabled\":%s,"
			 "\"text_enabled\":%s,"
			 "\"text_string\":\"%s\","
			 "\"logo_enabled\":%s}}",
			 st->cfg.font_size, st->cfg.time_enabled ? "true" : "false",
			 st->cfg.uptime_enabled ? "true" : "false",
			 st->cfg.text_enabled ? "true" : "false", st->cfg.text_string,
			 st->cfg.logo_enabled ? "true" : "false");
		return (int)strlen(resp_buf);
	}

	/* Default: status */
	int region_count = 0;
	for (int s = 0; s < st->stream_count; s++)
		for (int r = 0; r < ROD_MAX_REGIONS; r++)
			if (st->regions[s][r].enabled)
				region_count++;

	snprintf(resp_buf, resp_buf_size, "{\"status\":\"ok\",\"streams\":%d,\"regions\":%d}",
		 st->stream_count, region_count);
	return (int)strlen(resp_buf);
}

/* ── Entry point ── */

int main(int argc, char **argv)
{
	rss_daemon_ctx_t ctx;
	int ret = rss_daemon_init(&ctx, "rod", argc, argv);
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
	st.config = ctx.cfg;
	st.config_path = ctx.config_path;
	st.running = ctx.running;
	load_config(&st);

	/* Init fonts per stream — check for per-stream font_size override */
	for (int s = 0; s < st.stream_count; s++) {
		char key[32];
		snprintf(key, sizeof(key), "stream%d_font_size", s);
		int fs = rss_config_get_int(ctx.cfg, "osd", key, 0);
		if (fs <= 0) {
			/* No per-stream override — auto-scale from global */
			fs = st.cfg.font_size;
			if (s > 0) {
				fs = fs * st.stream_h[s] / st.stream_h[0];
				if (fs < 12)
					fs = 12;
			}
		}
		if (rod_render_init(&st, s, fs) < 0) {
			RSS_FATAL("font init failed for stream %d", s);
			goto cleanup;
		}
	}

	/* Load logo */
	if (st.cfg.logo_enabled) {
		if (rod_load_logo(st.cfg.logo_path, st.cfg.logo_width, st.cfg.logo_height,
				  &st.logo_data) == 0) {
			st.logo_data_size = st.cfg.logo_width * st.cfg.logo_height * 4;
			RSS_INFO("logo loaded: %s (%dx%d)", st.cfg.logo_path, st.cfg.logo_width,
				 st.cfg.logo_height);
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
			epoll_ctl(epoll_fd, EPOLL_CTL_ADD, ctrl_fd, &ev);
		}
	}

	/* Initial render of all regions */
	render_tick(&st);

	/* Main loop: 1Hz render, 100ms control socket poll */
	int64_t last_tick = rss_timestamp_us();

	while (*st.running) {
		int64_t now = rss_timestamp_us();
		if (now - last_tick >= 1000000) {
			render_tick(&st);
			last_tick = now;
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
		rod_render_deinit(&st, s);

	rss_config_free(ctx.cfg);
	rss_daemon_cleanup("rod");

	return 0;
}

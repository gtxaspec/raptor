/*
 * ric_main.c -- Raptor IR-Cut Day/Night Control Daemon
 *
 * Monitors ISP exposure via HAL and switches between day and night
 * modes with hysteresis. Controls IR-cut filter (single or dual GPIO)
 * and IR LED enable.
 *
 * Supports manual override via raptorctl: ric mode auto|day|night
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>

#include "ric.h"

static void load_config(ric_state_t *st)
{
	rss_config_t *cfg = st->config;
	ric_config_t *c = &st->cfg;

	c->enabled = rss_config_get_bool(cfg, "ircut", "enabled", true);

	const char *mode = rss_config_get_str(cfg, "ircut", "mode", "auto");
	if (strcmp(mode, "day") == 0)
		c->opmode = RIC_FORCE_DAY;
	else if (strcmp(mode, "night") == 0)
		c->opmode = RIC_FORCE_NIGHT;
	else
		c->opmode = RIC_AUTO;

	c->gpio_ircut = rss_config_get_int(cfg, "ircut", "gpio_ircut", -1);
	c->gpio_ircut2 = rss_config_get_int(cfg, "ircut", "gpio_ircut2", -1);
	c->gpio_irled = rss_config_get_int(cfg, "ircut", "gpio_irled", -1);

	/* Trigger mode: "luma" (default), "gain" (legacy), "adc" (photoresistor) */
	const char *trigger = rss_config_get_str(cfg, "ircut", "trigger", "luma");
	if (strcmp(trigger, "gain") == 0)
		c->trigger = RIC_TRIGGER_GAIN;
	else if (strcmp(trigger, "adc") == 0)
		c->trigger = RIC_TRIGGER_ADC;
	else
		c->trigger = RIC_TRIGGER_LUMA;

	/* Luma trigger thresholds */
	c->night_luma = rss_config_get_int(cfg, "ircut", "night_luma", 20);
	c->night_gain = rss_config_get_int(cfg, "ircut", "night_gain", 80000);
	c->day_gain_pct = rss_config_get_int(cfg, "ircut", "day_gain_pct", 25);

	/* ADC thresholds (trigger=adc) */
	c->adc_channel = rss_config_get_int(cfg, "ircut", "adc_channel", 0);
	c->adc_night = rss_config_get_int(cfg, "ircut", "adc_night", 200);
	c->adc_day = rss_config_get_int(cfg, "ircut", "adc_day", 600);

	/* Gain thresholds (legacy, only used when trigger=gain) */
	c->night_threshold = rss_config_get_int(cfg, "ircut", "night_threshold", 40000);
	c->day_threshold = rss_config_get_int(cfg, "ircut", "day_threshold", 25000);

	c->hysteresis_sec = rss_config_get_int(cfg, "ircut", "hysteresis_sec", 5);
	c->poll_interval_ms = rss_config_get_int(cfg, "ircut", "poll_interval_ms", 1000);
}

/*
 * Auto-discover GPIO pins from /etc/thingino.json when raptor.conf
 * leaves them at -1.  Format:
 *   "ircut": "57 58"   (one or two GPIOs, space-separated string)
 *   "ircut": 57         (single GPIO as integer)
 *   "ir850": 8          (IR LED GPIO)
 *   "ir940": 9          (IR LED GPIO, used if ir850 absent)
 */
#define THINGINO_JSON "/etc/thingino.json"

static void load_gpio_from_thingino_json(ric_config_t *c)
{
	if (c->gpio_ircut >= 0 && c->gpio_irled >= 0)
		return; /* already configured */

	FILE *f = fopen(THINGINO_JSON, "r");
	if (!f)
		return;

	char buf[2048];
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	if (n == 0)
		return;
	buf[n] = '\0';

	/* Find the "gpio" object — scan for keys within it.
	 * This is a simple substring search, not a full JSON parser,
	 * but sufficient for the flat key layout in thingino.json. */

	/* Parse ircut: can be string "57 58" or integer 57 */
	if (c->gpio_ircut < 0) {
		const char *p = strstr(buf, "\"ircut\"");
		if (p) {
			p += 7; /* skip "ircut" */
			/* skip whitespace and colon */
			while (*p == ' ' || *p == ':')
				p++;
			if (*p == '"') {
				/* String value: "57 58" or "57" */
				p++;
				c->gpio_ircut = (int)strtol(p, (char **)&p, 10);
				while (*p == ' ')
					p++;
				if (*p >= '0' && *p <= '9')
					c->gpio_ircut2 = (int)strtol(p, NULL, 10);
			} else if (*p >= '0' && *p <= '9') {
				/* Integer value */
				c->gpio_ircut = (int)strtol(p, NULL, 10);
			}
		}
	}

	/* Parse ir850 or ir940 for IR LED */
	if (c->gpio_irled < 0) {
		int val;
		if (rss_json_get_int(buf, "ir850", &val) == 0 && val >= 0) {
			c->gpio_irled = val;
		} else if (rss_json_get_int(buf, "ir940", &val) == 0 && val >= 0) {
			c->gpio_irled = val;
		}
	}

	if (c->gpio_ircut >= 0 || c->gpio_irled >= 0)
		RSS_INFO("GPIOs from %s: ircut=%d ircut2=%d irled=%d", THINGINO_JSON, c->gpio_ircut,
			 c->gpio_ircut2, c->gpio_irled);
}

/* ── Control socket ── */

static int ric_ctrl_handler(const char *cmd_json, char *resp_buf, int resp_buf_size, void *userdata)
{
	ric_state_t *st = userdata;

	int rc = rss_ctrl_handle_common(cmd_json, resp_buf, resp_buf_size, st->config,
					st->config_path);
	if (rc >= 0)
		return rc;

	if (strstr(cmd_json, "\"mode\"")) {
		char val[16];
		if (rss_json_get_str(cmd_json, "value", val, sizeof(val)) == 0) {
			if (strcmp(val, "day") == 0) {
				st->cfg.opmode = RIC_FORCE_DAY;
				ric_set_mode(st, RIC_MODE_DAY);
			} else if (strcmp(val, "night") == 0) {
				st->cfg.opmode = RIC_FORCE_NIGHT;
				ric_set_mode(st, RIC_MODE_NIGHT);
			} else {
				st->cfg.opmode = RIC_AUTO;
			}
			rss_config_set_str(st->config, "ircut", "mode", val);
		}
		snprintf(resp_buf, resp_buf_size,
			 "{\"status\":\"ok\",\"mode\":\"%s\",\"state\":\"%s\"}",
			 st->cfg.opmode == RIC_AUTO
				 ? "auto"
				 : (st->cfg.opmode == RIC_FORCE_DAY ? "day" : "night"),
			 st->current_mode == RIC_MODE_DAY ? "day" : "night");
		return (int)strlen(resp_buf);
	}

	if (strstr(cmd_json, "\"config-show\"")) {
		char exp_resp[256] = {0};
		rss_ctrl_send_command("/var/run/rss/rvd.sock", "{\"cmd\":\"get-exposure\"}",
				      exp_resp, sizeof(exp_resp), 1000);
		snprintf(resp_buf, resp_buf_size,
			 "{\"status\":\"ok\",\"mode\":\"%s\",\"state\":\"%s\","
			 "\"exposure\":%s,"
			 "\"night_threshold\":%d,\"day_threshold\":%d}",
			 st->cfg.opmode == RIC_AUTO
				 ? "auto"
				 : (st->cfg.opmode == RIC_FORCE_DAY ? "day" : "night"),
			 st->current_mode == RIC_MODE_DAY ? "day" : "night",
			 exp_resp[0] ? exp_resp : "null", st->cfg.night_threshold,
			 st->cfg.day_threshold);
		return (int)strlen(resp_buf);
	}

	/* Default: status */
	char exp_resp[256] = {0};
	rss_ctrl_send_command("/var/run/rss/rvd.sock", "{\"cmd\":\"get-exposure\"}", exp_resp,
			      sizeof(exp_resp), 1000);
	snprintf(resp_buf, resp_buf_size,
		 "{\"status\":\"ok\",\"mode\":\"%s\",\"state\":\"%s\",\"exposure\":%s}",
		 st->cfg.opmode == RIC_AUTO ? "auto"
					    : (st->cfg.opmode == RIC_FORCE_DAY ? "day" : "night"),
		 st->current_mode == RIC_MODE_DAY ? "day" : "night",
		 exp_resp[0] ? exp_resp : "null");
	return (int)strlen(resp_buf);
}

/* ── Entry point ── */

int main(int argc, char **argv)
{
	rss_daemon_ctx_t ctx;
	int ret = rss_daemon_init(&ctx, "ric", argc, argv);
	if (ret != 0)
		return ret < 0 ? 1 : 0;
	RSS_BANNER("ric");

	if (!rss_config_get_bool(ctx.cfg, "ircut", "enabled", true)) {
		RSS_INFO("IR-cut control disabled in config, exiting");
		rss_config_free(ctx.cfg);
		rss_daemon_cleanup("ric");
		return 0;
	}

	ric_state_t st = {0};
	st.config = ctx.cfg;
	st.config_path = ctx.config_path;
	st.running = ctx.running;
	st.current_mode = RIC_MODE_DAY;
	load_config(&st);
	load_gpio_from_thingino_json(&st.cfg);

	/* Wait for RVD control socket to be available */
	RSS_INFO("waiting for RVD...");
	for (int i = 0; i < 100 && *st.running; i++) {
		char resp[256];
		if (rss_ctrl_send_command("/var/run/rss/rvd.sock", "{\"cmd\":\"get-exposure\"}",
					  resp, sizeof(resp), 1000) >= 0) {
			RSS_INFO("RVD ready (%s)", resp);
			break;
		}
		usleep(500000);
	}

	/* Init GPIOs */
	ric_gpio_init(&st);

	/* Init ADC if trigger=adc; fall back to luma on failure */
	if (st.cfg.trigger == RIC_TRIGGER_ADC) {
		if (ric_adc_start(&st)) {
			st.adc_initialized = true;
		} else {
			RSS_WARN("ADC unavailable, falling back to luma trigger");
			st.cfg.trigger = RIC_TRIGGER_LUMA;
		}
	}

	/* Apply initial mode */
	if (st.cfg.opmode == RIC_FORCE_DAY)
		ric_set_mode(&st, RIC_MODE_DAY);
	else if (st.cfg.opmode == RIC_FORCE_NIGHT)
		ric_set_mode(&st, RIC_MODE_NIGHT);
	else
		ric_set_mode(&st, RIC_MODE_DAY); /* start in day, auto will adjust */

	/* Control socket */
	rss_mkdir_p("/var/run/rss");
	st.ctrl = rss_ctrl_listen("/var/run/rss/ric.sock");

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

	RSS_INFO("ric running (mode=%s, trigger=%s, gpio_ircut=%d, gpio_ircut2=%d, gpio_irled=%d)",
		 st.cfg.opmode == RIC_AUTO ? "auto"
					   : (st.cfg.opmode == RIC_FORCE_DAY ? "day" : "night"),
		 st.cfg.trigger == RIC_TRIGGER_LUMA ? "luma" : "gain", st.cfg.gpio_ircut,
		 st.cfg.gpio_ircut2, st.cfg.gpio_irled);

	/* Main loop: poll exposure + handle control socket */
	while (*st.running) {
		ric_poll_exposure(&st);

		if (epoll_fd >= 0) {
			struct epoll_event ev;
			int n = epoll_wait(epoll_fd, &ev, 1, st.cfg.poll_interval_ms);
			if (n > 0 && st.ctrl)
				rss_ctrl_accept_and_handle(st.ctrl, ric_ctrl_handler, &st);
		} else {
			usleep(st.cfg.poll_interval_ms * 1000);
		}
	}

	RSS_INFO("ric shutting down");

	ric_adc_cleanup(&st);

	if (epoll_fd >= 0)
		close(epoll_fd);
	if (st.ctrl)
		rss_ctrl_destroy(st.ctrl);

	rss_config_free(ctx.cfg);
	rss_daemon_cleanup("ric");

	return 0;
}

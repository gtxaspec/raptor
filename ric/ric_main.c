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
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/stat.h>

#include "ric.h"
#include "ric_json.h"

/*
 * The config file is a system boundary like the ctrl socket: the same
 * ranges set-threshold enforces apply here, clamped to the nearest
 * bound (the pulse_ms precedent). One line names what changed --
 * a silently corrected config is a debugging session.
 */
static char clamped_keys[128];

static int cfg_clamp(const char *key, int val, int lo, int hi)
{
	int out = val < lo ? lo : (val > hi ? hi : val);
	if (out != val) {
		size_t len = strlen(clamped_keys);
		snprintf(clamped_keys + len, sizeof(clamped_keys) - len, "%s%s", len ? ", " : "",
			 key);
	}
	return out;
}

static void load_config(ric_state_t *st)
{
	rss_config_t *cfg = st->cfg;
	ric_config_t *c = &st->settings;

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
	c->gpio_irled2 = rss_config_get_int(cfg, "ircut", "gpio_irled2", -1);
	c->ir850_enabled = rss_config_get_bool(cfg, "ircut", "ir850", true);
	/* Probed before the get, because the getters store their default into
	 * the config on a miss (for config-get-section) and the key would
	 * always look present afterwards. The 940-only default below needs
	 * to know the difference. */
	c->ir940_explicit = rss_config_get_str(cfg, "ircut", "ir940", NULL) != NULL;
	c->ir940_enabled = rss_config_get_bool(cfg, "ircut", "ir940", false);

	/* Trigger mode: "luma" (default), "gain" (legacy), "adc", "photo" */
	const char *trigger = rss_config_get_str(cfg, "ircut", "trigger", "luma");
	if (strcmp(trigger, "gain") == 0)
		c->trigger = RIC_TRIGGER_GAIN;
	else if (strcmp(trigger, "adc") == 0)
		c->trigger = RIC_TRIGGER_ADC;
	else if (strcmp(trigger, "photo") == 0)
		c->trigger = RIC_TRIGGER_PHOTO;
	else
		c->trigger = RIC_TRIGGER_LUMA;

	/* Luma trigger thresholds */
	c->night_luma =
		cfg_clamp("night_luma", rss_config_get_int(cfg, "ircut", "night_luma", 20), 0, 255);
	c->night_gain = cfg_clamp(
		"night_gain", rss_config_get_int(cfg, "ircut", "night_gain", 80000), 0, INT_MAX);
	c->day_gain_pct = cfg_clamp("day_gain_pct",
				    rss_config_get_int(cfg, "ircut", "day_gain_pct", 25), 1, 100);
	c->probe_gain_pct = cfg_clamp(
		"probe_gain_pct", rss_config_get_int(cfg, "ircut", "probe_gain_pct", 90), 0, 99);
	c->probe_holdoff_sec =
		cfg_clamp("probe_holdoff_sec",
			  rss_config_get_int(cfg, "ircut", "probe_holdoff_sec", 60), 1, 86400);
	c->probe_recheck_sec =
		cfg_clamp("probe_recheck_sec",
			  rss_config_get_int(cfg, "ircut", "probe_recheck_sec", 600), 0, 86400);

	/* ADC thresholds (trigger=adc) */
	c->adc_channel = rss_config_get_int(cfg, "ircut", "adc_channel", 0);
	c->adc_night = rss_config_get_int(cfg, "ircut", "adc_night", 200);
	c->adc_day = rss_config_get_int(cfg, "ircut", "adc_day", 600);

	/* Photo trigger thresholds (high ev = dark on Ingenic) */
	c->photo.ev_night = (uint32_t)rss_config_get_int(cfg, "ircut", "photo_ev_night", 50000);
	c->photo.ev_deep = (uint32_t)rss_config_get_int(cfg, "ircut", "photo_ev_deep", 150000);
	c->photo.ev_day = (uint32_t)rss_config_get_int(cfg, "ircut", "photo_ev_day", 5000);
	c->photo.rgain_rec = (uint16_t)rss_config_get_int(cfg, "ircut", "photo_rgain_rec", 0);
	c->photo.bgain_rec = (uint16_t)rss_config_get_int(cfg, "ircut", "photo_bgain_rec", 0);

	/* Gain thresholds (legacy, only used when trigger=gain) */
	c->night_threshold =
		cfg_clamp("night_threshold",
			  rss_config_get_int(cfg, "ircut", "night_threshold", 40000), 0, INT_MAX);
	c->day_threshold =
		cfg_clamp("day_threshold", rss_config_get_int(cfg, "ircut", "day_threshold", 25000),
			  0, INT_MAX);

	c->hysteresis_sec = cfg_clamp(
		"hysteresis_sec", rss_config_get_int(cfg, "ircut", "hysteresis_sec", 5), 1, 300);
	int default_poll = (c->trigger == RIC_TRIGGER_PHOTO) ? 100 : 1000;
	c->poll_interval_ms = cfg_clamp(
		"poll_interval_ms",
		rss_config_get_int(cfg, "ircut", "poll_interval_ms", default_poll), 50, 10000);

	/* Dual-GPIO coil pulse. 10ms is what the thingino ircut script has
	 * driven the whole fleet with since thingino-daynight existed; both
	 * 10ms and 100ms measured 20/20 reliable on a dual-GPIO Wyze Cam3,
	 * so the default follows the fleet. Clamped: a zero pulse moves no
	 * filter, and holding the coil for seconds is a heater. */
	c->pulse_ms =
		cfg_clamp("pulse_ms", rss_config_get_int(cfg, "ircut", "pulse_ms", 10), 1, 1000);

	if (clamped_keys[0])
		RSS_WARN("config values out of range, clamped: %s", clamped_keys);

	/* The photo state machine assumes bright < dark < very dark;
	 * inverted thresholds silently break the deep-count logic. */
	if (c->trigger == RIC_TRIGGER_PHOTO &&
	    (c->photo.ev_day >= c->photo.ev_night || c->photo.ev_night >= c->photo.ev_deep))
		RSS_WARN("photo thresholds out of order (need ev_day %u < ev_night %u < ev_deep "
			 "%u) -- detection will misbehave",
			 c->photo.ev_day, c->photo.ev_night, c->photo.ev_deep);
}

/* Pins may also be auto-discovered from the thingino device file
 * when raptor.conf leaves them at -1 (see ric_json.c). */
#define THINGINO_JSON "/etc/thingino.json"

/* ── Control socket ── */

static int ric_ctrl_handler(const char *cmd_json, char *resp_buf, int resp_buf_size, void *userdata)
{
	ric_state_t *st = userdata;

	int rc =
		rss_ctrl_handle_common(cmd_json, resp_buf, resp_buf_size, st->cfg, st->config_path);
	if (rc >= 0)
		return rc;

	char cmd[64];
	if (rss_json_get_str(cmd_json, "cmd", cmd, sizeof(cmd)) != 0)
		return rss_ctrl_resp_error(resp_buf, resp_buf_size, "missing cmd");

	if (strcmp(cmd, "isp-mode") == 0) {
		/* ISP running mode only — no GPIO/IR-cut toggling */
		char val[16];
		const char *isp_state = "day";
		if (rss_json_get_str(cmd_json, "value", val, sizeof(val)) == 0) {
			ric_mode_t m = strcmp(val, "night") == 0 ? RIC_MODE_NIGHT : RIC_MODE_DAY;
			ric_set_isp_mode(m);
			isp_state = val;
			RSS_INFO("ISP mode set to %s (GPIO unchanged)", val);
		}
		cJSON *r = cJSON_CreateObject();
		cJSON_AddStringToObject(r, "status", "ok");
		cJSON_AddStringToObject(r, "isp_mode", isp_state);
		cJSON_AddStringToObject(r, "hw_state",
					st->current_mode == RIC_MODE_DAY ? "day" : "night");
		return rss_ctrl_resp_json(resp_buf, resp_buf_size, r);
	}

	/*
	 * Manual hardware control, one piece at a time. Deliberately does
	 * not touch current_mode or opmode: this is a bench tool, and the
	 * next automatic transition reasserts whatever auto wants -- the
	 * response carries the operating mode so the caller knows whether
	 * that can happen. The LED commands also ignore the ir850/ir940
	 * enable flags on purpose: those gate automatic behavior, while a
	 * manual command is explicit intent (lighting a disabled bank from
	 * the shell is exactly what bench debugging needs).
	 *
	 * These handlers knowingly block the ctrl socket: an ircut move
	 * holds the coil for pulse_ms (capped 1s) and a forced mode adds
	 * an ISP call with a 2s timeout. Worst case sits inside the 5s
	 * send budget, and the hardware pulse cannot be shortened.
	 */
	if (strcmp(cmd, "ircut") == 0) {
		char val[8];
		if (rss_json_get_str(cmd_json, "value", val, sizeof(val)) != 0 ||
		    (strcmp(val, "day") != 0 && strcmp(val, "night") != 0))
			return rss_ctrl_resp_error(resp_buf, resp_buf_size, "need value day|night");
		if (ric_ircut_drive(st, strcmp(val, "night") == 0 ? RIC_MODE_NIGHT : RIC_MODE_DAY) <
		    0)
			return rss_ctrl_resp_error(resp_buf, resp_buf_size,
						   "no ircut pins configured");
		RSS_INFO("manual ircut -> %s", val);
		cJSON *r = cJSON_CreateObject();
		cJSON_AddStringToObject(r, "status", "ok");
		cJSON_AddStringToObject(r, "ircut", val);
		cJSON_AddStringToObject(r, "mode",
					st->settings.opmode == RIC_AUTO	       ? "auto"
					: st->settings.opmode == RIC_FORCE_DAY ? "day"
									       : "night");
		return rss_ctrl_resp_json(resp_buf, resp_buf_size, r);
	}

	if (strcmp(cmd, "ir850") == 0 || strcmp(cmd, "ir940") == 0) {
		char val[8];
		bool bank940 = cmd[2] == '9';
		if (rss_json_get_str(cmd_json, "value", val, sizeof(val)) != 0 ||
		    (strcmp(val, "on") != 0 && strcmp(val, "off") != 0))
			return rss_ctrl_resp_error(resp_buf, resp_buf_size, "need value on|off");
		if (ric_irled_drive(st, bank940, strcmp(val, "on") == 0) < 0)
			return rss_ctrl_resp_error(resp_buf, resp_buf_size,
						   bank940 ? "no ir940 pin configured"
							   : "no ir850 pin configured");
		RSS_INFO("manual %s -> %s", cmd, val);
		cJSON *r = cJSON_CreateObject();
		cJSON_AddStringToObject(r, "status", "ok");
		cJSON_AddStringToObject(r, cmd, val);
		cJSON_AddStringToObject(r, "mode",
					st->settings.opmode == RIC_AUTO	       ? "auto"
					: st->settings.opmode == RIC_FORCE_DAY ? "day"
									       : "night");
		return rss_ctrl_resp_json(resp_buf, resp_buf_size, r);
	}

	if (strcmp(cmd, "mode") == 0) {
		char val[16];
		if (rss_json_get_str(cmd_json, "value", val, sizeof(val)) == 0) {
			if (strcmp(val, "day") == 0) {
				st->settings.opmode = RIC_FORCE_DAY;
				ric_force_mode(st, RIC_MODE_DAY);
			} else if (strcmp(val, "night") == 0) {
				st->settings.opmode = RIC_FORCE_NIGHT;
				ric_force_mode(st, RIC_MODE_NIGHT);
			} else if (strcmp(val, "auto") == 0) {
				st->settings.opmode = RIC_AUTO;
			} else {
				return rss_ctrl_resp_error(resp_buf, resp_buf_size,
							   "need value auto|day|night");
			}
			rss_config_set_str(st->cfg, "ircut", "mode", val);
		}
		cJSON *r = cJSON_CreateObject();
		cJSON_AddStringToObject(r, "status", "ok");
		cJSON_AddStringToObject(r, "mode",
					st->settings.opmode == RIC_AUTO	       ? "auto"
					: st->settings.opmode == RIC_FORCE_DAY ? "day"
									       : "night");
		cJSON_AddStringToObject(r, "state",
					st->current_mode == RIC_MODE_DAY ? "day" : "night");
		return rss_ctrl_resp_json(resp_buf, resp_buf_size, r);
	}

	if (strcmp(cmd, "set-threshold") == 0) {
		char key[32] = "";
		int val;
		if (rss_json_get_str(cmd_json, "key", key, sizeof(key)) != 0 ||
		    rss_json_get_int(cmd_json, "value", &val) != 0)
			return rss_ctrl_resp_error(resp_buf, resp_buf_size, "need key and value");

		ric_config_t *c = &st->settings;
		const char *cfg_key = NULL;
		if (strcmp(key, "night_luma") == 0) {
			if (val < 0 || val > 255)
				return rss_ctrl_resp_error(resp_buf, resp_buf_size, "range 0-255");
			c->night_luma = val;
			cfg_key = "night_luma";
		} else if (strcmp(key, "night_gain") == 0) {
			if (val < 0)
				return rss_ctrl_resp_error(resp_buf, resp_buf_size, "must be >= 0");
			c->night_gain = val;
			cfg_key = "night_gain";
		} else if (strcmp(key, "day_gain_pct") == 0) {
			if (val < 1 || val > 100)
				return rss_ctrl_resp_error(resp_buf, resp_buf_size, "range 1-100");
			c->day_gain_pct = val;
			cfg_key = "day_gain_pct";
		} else if (strcmp(key, "night_threshold") == 0) {
			if (val < 0)
				return rss_ctrl_resp_error(resp_buf, resp_buf_size, "must be >= 0");
			c->night_threshold = val;
			cfg_key = "night_threshold";
		} else if (strcmp(key, "day_threshold") == 0) {
			if (val < 0)
				return rss_ctrl_resp_error(resp_buf, resp_buf_size, "must be >= 0");
			c->day_threshold = val;
			cfg_key = "day_threshold";
		} else if (strcmp(key, "hysteresis_sec") == 0) {
			if (val < 1 || val > 300)
				return rss_ctrl_resp_error(resp_buf, resp_buf_size, "range 1-300");
			c->hysteresis_sec = val;
			cfg_key = "hysteresis_sec";
		} else if (strcmp(key, "poll_interval_ms") == 0) {
			if (val < 50 || val > 10000)
				return rss_ctrl_resp_error(resp_buf, resp_buf_size,
							   "range 50-10000");
			c->poll_interval_ms = val;
			cfg_key = "poll_interval_ms";
		} else if (strcmp(key, "probe_gain_pct") == 0) {
			if (val < 0 || val > 99)
				return rss_ctrl_resp_error(resp_buf, resp_buf_size, "range 0-99");
			c->probe_gain_pct = val;
			cfg_key = "probe_gain_pct";
		} else if (strcmp(key, "probe_holdoff_sec") == 0) {
			if (val < 1 || val > 86400)
				return rss_ctrl_resp_error(resp_buf, resp_buf_size,
							   "range 1-86400");
			c->probe_holdoff_sec = val;
			cfg_key = "probe_holdoff_sec";
		} else if (strcmp(key, "probe_recheck_sec") == 0) {
			if (val < 0 || val > 86400)
				return rss_ctrl_resp_error(resp_buf, resp_buf_size,
							   "range 0-86400");
			c->probe_recheck_sec = val;
			/* Take effect now: a countdown armed with the old
			 * interval would ignore a shorter one until the next
			 * night entry. */
			{
				int polls = val * 1000 /
					    (c->poll_interval_ms > 0 ? c->poll_interval_ms : 1000);
				if (st->probe_recheck_polls > polls)
					st->probe_recheck_polls = polls;
			}
			cfg_key = "probe_recheck_sec";
		} else if (strcmp(key, "photo_ev_night") == 0) {
			if (val < 0)
				return rss_ctrl_resp_error(resp_buf, resp_buf_size, "must be >= 0");
			c->photo.ev_night = (uint32_t)val;
			cfg_key = "photo_ev_night";
		} else if (strcmp(key, "photo_ev_deep") == 0) {
			if (val < 0)
				return rss_ctrl_resp_error(resp_buf, resp_buf_size, "must be >= 0");
			c->photo.ev_deep = (uint32_t)val;
			cfg_key = "photo_ev_deep";
		} else if (strcmp(key, "photo_ev_day") == 0) {
			if (val < 0)
				return rss_ctrl_resp_error(resp_buf, resp_buf_size, "must be >= 0");
			c->photo.ev_day = (uint32_t)val;
			cfg_key = "photo_ev_day";
		} else if (strcmp(key, "photo_rgain_rec") == 0) {
			if (val < 0 || val > 65535)
				return rss_ctrl_resp_error(resp_buf, resp_buf_size,
							   "range 0-65535");
			c->photo.rgain_rec = (uint16_t)val;
			cfg_key = "photo_rgain_rec";
		} else if (strcmp(key, "photo_bgain_rec") == 0) {
			if (val < 0 || val > 65535)
				return rss_ctrl_resp_error(resp_buf, resp_buf_size,
							   "range 0-65535");
			c->photo.bgain_rec = (uint16_t)val;
			cfg_key = "photo_bgain_rec";
		} else {
			return rss_ctrl_resp_error(resp_buf, resp_buf_size, "unknown key");
		}

		char valbuf[16];
		snprintf(valbuf, sizeof(valbuf), "%d", val);
		rss_config_set_str(st->cfg, "ircut", cfg_key, valbuf);
		RSS_INFO("threshold %s set to %d", key, val);
		return rss_ctrl_resp_ok(resp_buf, resp_buf_size);
	}

	if (strcmp(cmd, "get-thresholds") == 0) {
		ric_config_t *c = &st->settings;
		cJSON *r = cJSON_CreateObject();
		if (!r)
			return rss_ctrl_resp_error(resp_buf, resp_buf_size, "alloc");
		cJSON_AddStringToObject(r, "status", "ok");
		cJSON_AddStringToObject(r, "trigger",
					c->trigger == RIC_TRIGGER_LUMA	 ? "luma"
					: c->trigger == RIC_TRIGGER_GAIN ? "gain"
					: c->trigger == RIC_TRIGGER_ADC	 ? "adc"
									 : "photo");
		cJSON_AddNumberToObject(r, "night_luma", c->night_luma);
		cJSON_AddNumberToObject(r, "night_gain", c->night_gain);
		cJSON_AddNumberToObject(r, "day_gain_pct", c->day_gain_pct);
		cJSON_AddNumberToObject(r, "night_threshold", c->night_threshold);
		cJSON_AddNumberToObject(r, "day_threshold", c->day_threshold);
		cJSON_AddNumberToObject(r, "hysteresis_sec", c->hysteresis_sec);
		cJSON_AddNumberToObject(r, "poll_interval_ms", c->poll_interval_ms);
		cJSON_AddNumberToObject(r, "photo_ev_night", c->photo.ev_night);
		cJSON_AddNumberToObject(r, "photo_ev_deep", c->photo.ev_deep);
		cJSON_AddNumberToObject(r, "photo_ev_day", c->photo.ev_day);
		cJSON_AddNumberToObject(r, "photo_rgain_rec", c->photo.rgain_rec);
		cJSON_AddNumberToObject(r, "photo_bgain_rec", c->photo.bgain_rec);
		return rss_ctrl_resp_json(resp_buf, resp_buf_size, r);
	}

	if (strcmp(cmd, "config-show") == 0) {
		char exp_resp[256] = {0};
		rss_ctrl_send_command(RSS_RUN_DIR "/rvd.sock", "{\"cmd\":\"get-exposure\"}",
				      exp_resp, sizeof(exp_resp), 1000);
		cJSON *r = cJSON_CreateObject();
		if (!r)
			return rss_ctrl_resp_error(resp_buf, resp_buf_size, "alloc");
		cJSON_AddStringToObject(r, "status", "ok");
		cJSON_AddStringToObject(r, "mode",
					st->settings.opmode == RIC_AUTO	       ? "auto"
					: st->settings.opmode == RIC_FORCE_DAY ? "day"
									       : "night");
		cJSON_AddStringToObject(r, "state",
					st->current_mode == RIC_MODE_DAY ? "day" : "night");
		cJSON *sub = exp_resp[0] ? cJSON_Parse(exp_resp) : NULL;
		if (sub)
			cJSON_AddItemToObject(r, "exposure", sub);
		cJSON_AddNumberToObject(r, "night_luma", st->settings.night_luma);
		cJSON_AddNumberToObject(r, "night_gain", st->settings.night_gain);
		cJSON_AddNumberToObject(r, "day_gain_pct", st->settings.day_gain_pct);
		cJSON_AddNumberToObject(r, "night_threshold", st->settings.night_threshold);
		cJSON_AddNumberToObject(r, "day_threshold", st->settings.day_threshold);
		cJSON_AddNumberToObject(r, "hysteresis_sec", st->settings.hysteresis_sec);
		cJSON_AddNumberToObject(r, "poll_interval_ms", st->settings.poll_interval_ms);
		return rss_ctrl_resp_json(resp_buf, resp_buf_size, r);
	}

	if (strcmp(cmd, "status") == 0) {
		char exp_resp[256] = {0};
		rss_ctrl_send_command(RSS_RUN_DIR "/rvd.sock", "{\"cmd\":\"get-exposure\"}",
				      exp_resp, sizeof(exp_resp), 1000);
		cJSON *r = cJSON_CreateObject();
		cJSON_AddStringToObject(r, "status", "ok");
		cJSON_AddStringToObject(r, "mode",
					st->settings.opmode == RIC_AUTO	       ? "auto"
					: st->settings.opmode == RIC_FORCE_DAY ? "day"
									       : "night");
		cJSON_AddStringToObject(r, "state",
					st->current_mode == RIC_MODE_DAY ? "day" : "night");
		cJSON *sub = exp_resp[0] ? cJSON_Parse(exp_resp) : NULL;
		if (sub)
			cJSON_AddItemToObject(r, "exposure", sub);
		return rss_ctrl_resp_json(resp_buf, resp_buf_size, r);
	}

	return rss_ctrl_resp_error(resp_buf, resp_buf_size, "unknown command");
}

/* ── Entry point ── */

int main(int argc, char **argv)
{
	rss_daemon_ctx_t ctx;
	int ret = rss_daemon_init(&ctx, "ric", argc, argv, NULL);
	if (ret != 0)
		return ret < 0 ? 1 : 0;
	ric_state_t st = {0};
	st.adc_fd = -1;
	int epoll_fd = -1;

	if (!rss_config_get_bool(ctx.cfg, "ircut", "enabled", true)) {
		RSS_INFO("IR-cut control disabled in config, exiting");
		goto cleanup;
	}

	st.cfg = ctx.cfg;
	st.config_path = ctx.config_path;
	st.running = ctx.running;
	st.current_mode = RIC_MODE_DAY;
	load_config(&st);
	ric_json_gpio_load(&st.settings, THINGINO_JSON);

	/* The only IR bank a board has must light by default. ir940 defaults
	 * to off as the opt-in second bank, which left a 940nm-only board
	 * blind at night with a clean log unless a config said otherwise --
	 * found in the field on a wuuk y0510. Decided here, after discovery,
	 * because that is when the bank topology is finally known; only the
	 * shipped default is overridden, an explicit ir940 key keeps its say. */
	if (st.settings.gpio_irled < 0 && st.settings.gpio_irled2 >= 0 &&
	    !st.settings.ir940_enabled && !st.settings.ir940_explicit) {
		st.settings.ir940_enabled = true;
		RSS_INFO("940nm is the only IR bank; enabling it "
			 "(set ircut.ir940 = false to override)");
	}

	/* Wait for RVD control socket to be available */
	RSS_DEBUG("waiting for RVD...");
	for (int i = 0; i < 100 && *st.running; i++) {
		char resp[256];
		if (rss_ctrl_send_command(RSS_RUN_DIR "/rvd.sock", "{\"cmd\":\"get-exposure\"}",
					  resp, sizeof(resp), 1000) >= 0) {
			RSS_INFO("RVD ready (%s)", resp);
			break;
		}
		usleep(500000);
	}

	/* Init GPIOs */
	ric_gpio_init(&st);

	/* Init ADC if trigger=adc; fall back to luma on failure */
	if (st.settings.trigger == RIC_TRIGGER_ADC) {
		if (ric_adc_start(&st)) {
			st.adc_initialized = true;
		} else {
			RSS_WARN("ADC unavailable, falling back to luma trigger");
			st.settings.trigger = RIC_TRIGGER_LUMA;
		}
	}

	/* Init photo mode state */
	if (st.settings.trigger == RIC_TRIGGER_PHOTO) {
		ric_photo_reset(&st.photo, PHOTO_PHASE_NIGHT_DETECT);
		if (st.settings.photo.rgain_rec > 0 && st.settings.photo.bgain_rec > 0) {
			st.photo.rgain_base = st.settings.photo.rgain_rec;
			st.photo.bgain_base = st.settings.photo.bgain_rec;
			st.photo.calibrated = true;
			RSS_INFO("photo AWB baseline from config: rg=%u bg=%u", st.photo.rgain_base,
				 st.photo.bgain_base);
		}
	}

	/* Apply initial mode -- force GPIOs to known state at startup */
	st.current_mode = RIC_MODE_UNSET;
	if (st.settings.opmode == RIC_FORCE_DAY)
		ric_set_mode(&st, RIC_MODE_DAY);
	else if (st.settings.opmode == RIC_FORCE_NIGHT)
		ric_set_mode(&st, RIC_MODE_NIGHT);
	else
		ric_set_mode(&st, RIC_MODE_DAY);

	/* Control socket */
	rss_mkdir_p(RSS_RUN_DIR);
	st.ctrl = rss_ctrl_listen(RSS_RUN_DIR "/ric.sock");

	int ctrl_fd = st.ctrl ? rss_ctrl_get_fd(st.ctrl) : -1;
	if (ctrl_fd >= 0) {
		epoll_fd = epoll_create1(EPOLL_CLOEXEC);
		if (epoll_fd >= 0) {
			struct epoll_event ev = {.events = EPOLLIN, .data.fd = ctrl_fd};
			if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, ctrl_fd, &ev) < 0)
				RSS_ERROR("epoll_ctl add ctrl_fd: %s", strerror(errno));
		}
	}

	static const char *trigger_names[] = {"luma", "gain", "adc", "photo"};
	RSS_INFO("ric running (mode=%s, trigger=%s, gpio_ircut=%d, gpio_ircut2=%d, gpio_irled=%d, "
		 "gpio_irled2=%d)",
		 st.settings.opmode == RIC_AUTO
			 ? "auto"
			 : (st.settings.opmode == RIC_FORCE_DAY ? "day" : "night"),
		 trigger_names[st.settings.trigger], st.settings.gpio_ircut,
		 st.settings.gpio_ircut2, st.settings.gpio_irled, st.settings.gpio_irled2);
	if (st.settings.trigger == RIC_TRIGGER_ADC) {
		RSS_DEBUG("  adc: channel=%d night=%d day=%d", st.settings.adc_channel,
			  st.settings.adc_night, st.settings.adc_day);
	} else if (st.settings.trigger == RIC_TRIGGER_PHOTO) {
		RSS_DEBUG("  photo: ev_night=%u ev_deep=%u ev_day=%u rg=%u bg=%u (0=auto)",
			  st.settings.photo.ev_night, st.settings.photo.ev_deep,
			  st.settings.photo.ev_day, st.settings.photo.rgain_rec,
			  st.settings.photo.bgain_rec);
	} else if (st.settings.trigger == RIC_TRIGGER_LUMA) {
		RSS_DEBUG("  luma: night_luma=%d night_gain=%d day_gain_pct=%d",
			  st.settings.night_luma, st.settings.night_gain, st.settings.day_gain_pct);
	} else {
		RSS_DEBUG("  gain: night=%d day=%d", st.settings.night_threshold,
			  st.settings.day_threshold);
	}
	RSS_DEBUG("  hysteresis=%ds poll=%dms", st.settings.hysteresis_sec,
		  st.settings.poll_interval_ms);

	/* Main loop: poll exposure + handle control socket */
	while (*st.running) {
		ric_poll_exposure(&st);

		if (epoll_fd >= 0) {
			struct epoll_event ev;
			int n = epoll_wait(epoll_fd, &ev, 1, st.settings.poll_interval_ms);
			if (n > 0 && st.ctrl)
				rss_ctrl_accept_and_handle(st.ctrl, ric_ctrl_handler, &st);
		} else {
			usleep(st.settings.poll_interval_ms * 1000);
		}
	}

	RSS_INFO("ric shutting down");

cleanup:
	ric_adc_cleanup(&st);
	if (epoll_fd >= 0)
		close(epoll_fd);
	if (st.ctrl)
		rss_ctrl_destroy(st.ctrl);
	rss_config_free(ctx.cfg);
	rss_daemon_cleanup("ric");
	return 0;
}

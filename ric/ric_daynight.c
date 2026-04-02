/*
 * ric_daynight.c -- IR-cut filter and day/night mode control
 *
 * Supports single GPIO (one pin toggles) and dual GPIO (two pins
 * pulsed for motor-driven IR-cut filters).
 *
 * ADC support uses dlopen for libsysutils.so — graceful fallback
 * to luma mode if the library or hardware is unavailable.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>

#include "ric.h"

/* ── ADC via libsysutils (runtime-loaded) ── */

static void *adc_lib;
static int (*adc_init)(void);
static int (*adc_exit)(void);
static int (*adc_enable)(uint32_t chn);
static int (*adc_disable)(uint32_t chn);
static int (*adc_get_value)(uint32_t chn, int *value);

static bool adc_load(void)
{
	if (adc_lib)
		return true;

	adc_lib = dlopen("libsysutils.so", RTLD_LAZY);
	if (!adc_lib) {
		RSS_WARN("ADC: libsysutils.so not available (%s)", dlerror());
		return false;
	}

	adc_init = dlsym(adc_lib, "SU_ADC_Init");
	adc_exit = dlsym(adc_lib, "SU_ADC_Exit");
	adc_enable = dlsym(adc_lib, "SU_ADC_EnableChn");
	adc_disable = dlsym(adc_lib, "SU_ADC_DisableChn");
	adc_get_value = dlsym(adc_lib, "SU_ADC_GetChnValue");

	if (!adc_init || !adc_get_value || !adc_enable) {
		RSS_WARN("ADC: missing symbols in libsysutils.so");
		dlclose(adc_lib);
		adc_lib = NULL;
		return false;
	}
	return true;
}

bool ric_adc_start(ric_state_t *st)
{
	if (!adc_load())
		return false;

	int channel = st->cfg.adc_channel;
	if (adc_init() != 0) {
		RSS_WARN("ADC: SU_ADC_Init failed");
		return false;
	}
	if (adc_enable((uint32_t)channel) != 0) {
		RSS_WARN("ADC: SU_ADC_EnableChn(%d) failed", channel);
		if (adc_exit)
			adc_exit();
		return false;
	}

	RSS_INFO("ADC: channel %d initialized", channel);
	return true;
}

static int adc_read(int channel)
{
	int value = -1;
	if (adc_get_value && adc_get_value((uint32_t)channel, &value) != 0)
		return -1;
	return value;
}

void ric_adc_cleanup(ric_state_t *st)
{
	if (st->adc_initialized) {
		if (adc_disable)
			adc_disable((uint32_t)st->cfg.adc_channel);
		if (adc_exit)
			adc_exit();
		st->adc_initialized = false;
	}
	if (adc_lib) {
		dlclose(adc_lib);
		adc_lib = NULL;
	}
}

/* Export a GPIO pin via sysfs (ignore errors if already exported) */
static void gpio_export(int pin)
{
	if (pin < 0)
		return;
	char buf[16];
	int len = snprintf(buf, sizeof(buf), "%d", pin);
	int fd = open("/sys/class/gpio/export", O_WRONLY);
	if (fd >= 0) {
		write(fd, buf, len);
		close(fd);
	}
	/* Set direction to output */
	char path[64];
	snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", pin);
	fd = open(path, O_WRONLY);
	if (fd >= 0) {
		write(fd, "out", 3);
		close(fd);
	}
}

static void gpio_set(int pin, int value)
{
	if (pin < 0)
		return;
	char path[64];
	snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin);
	int fd = open(path, O_WRONLY);
	if (fd >= 0) {
		write(fd, value ? "1" : "0", 1);
		close(fd);
	}
}

void ric_gpio_init(ric_state_t *st)
{
	gpio_export(st->cfg.gpio_ircut);
	if (st->cfg.gpio_ircut2 >= 0)
		gpio_export(st->cfg.gpio_ircut2);
	if (st->cfg.gpio_irled >= 0)
		gpio_export(st->cfg.gpio_irled);
}

/*
 * Set the IR-cut filter and ISP running mode.
 *
 * Single GPIO mode: pin high = day (filter closed), low = night (filter open).
 * Dual GPIO mode: pulse pin1/pin2 for 100ms, then both low (motor driver).
 */
void ric_set_mode(ric_state_t *st, ric_mode_t mode)
{
	if (mode == st->current_mode)
		return;

	if (mode == RIC_MODE_NIGHT) {
		/* Night: open IR-cut filter, enable IR LEDs, ISP night mode */
		if (st->cfg.gpio_ircut2 >= 0) {
			/* Dual GPIO: pulse for motor */
			gpio_set(st->cfg.gpio_ircut, 0);
			gpio_set(st->cfg.gpio_ircut2, 1);
			usleep(100000); /* 100ms pulse */
			gpio_set(st->cfg.gpio_ircut, 0);
			gpio_set(st->cfg.gpio_ircut2, 0);
		} else {
			/* Single GPIO */
			gpio_set(st->cfg.gpio_ircut, 0);
		}
		if (st->cfg.gpio_irled >= 0)
			gpio_set(st->cfg.gpio_irled, 1);
		{
			char resp[128];
			rss_ctrl_send_command("/var/run/rss/rvd.sock",
					      "{\"cmd\":\"set-running-mode\",\"value\":\"night\"}",
					      resp, sizeof(resp), 2000);
		}
		RSS_INFO("switched to NIGHT mode");
	} else {
		/* Day: close IR-cut filter, disable IR LEDs, ISP day mode */
		if (st->cfg.gpio_ircut2 >= 0) {
			/* Dual GPIO: pulse for motor */
			gpio_set(st->cfg.gpio_ircut, 1);
			gpio_set(st->cfg.gpio_ircut2, 0);
			usleep(100000); /* 100ms pulse */
			gpio_set(st->cfg.gpio_ircut, 0);
			gpio_set(st->cfg.gpio_ircut2, 0);
		} else {
			/* Single GPIO */
			gpio_set(st->cfg.gpio_ircut, 1);
		}
		if (st->cfg.gpio_irled >= 0)
			gpio_set(st->cfg.gpio_irled, 0);
		{
			char resp[128];
			rss_ctrl_send_command("/var/run/rss/rvd.sock",
					      "{\"cmd\":\"set-running-mode\",\"value\":\"day\"}",
					      resp, sizeof(resp), 2000);
		}
		RSS_INFO("switched to DAY mode");
	}

	st->current_mode = mode;
	st->day_count = 0;
	st->night_count = 0;

	/* Cooldown: wait 3 polls for IR LEDs / ISP to stabilize before
	 * evaluating transitions. After cooldown in night mode, the gain
	 * baseline is sampled for the night→day transition algorithm. */
	st->cooldown_remaining = 3;
	if (mode == RIC_MODE_DAY)
		st->night_gain_baseline = 0;
}

/*
 * Poll ISP exposure and decide day/night transition.
 * Uses total_gain with hysteresis debounce.
 */
/* Parse integer from simple JSON: "key":123 */
static int json_parse_uint(const char *json, const char *key, uint32_t *out)
{
	char pattern[64];
	snprintf(pattern, sizeof(pattern), "\"%s\":", key);
	const char *p = strstr(json, pattern);
	if (!p)
		return -1;
	p += strlen(pattern);
	*out = (uint32_t)strtoul(p, NULL, 10);
	return 0;
}

void ric_poll_exposure(ric_state_t *st)
{
	if (st->cfg.opmode != RIC_AUTO)
		return;

	/* Query RVD for ISP exposure data via control socket */
	char resp[256];
	int ret = rss_ctrl_send_command("/var/run/rss/rvd.sock", "{\"cmd\":\"get-exposure\"}", resp,
					sizeof(resp), 1000);
	if (ret < 0)
		return;

	uint32_t total_gain = 0, ae_luma = 0;
	json_parse_uint(resp, "total_gain", &total_gain);
	json_parse_uint(resp, "ae_luma", &ae_luma);

	/* Cooldown after mode switch: wait for IR LEDs / ISP to stabilize.
	 * After cooldown in night mode, sample total_gain as baseline for
	 * the auto-calibrating night→day transition. */
	if (st->cooldown_remaining > 0) {
		st->cooldown_remaining--;
		if (st->cooldown_remaining == 0 && st->current_mode == RIC_MODE_NIGHT) {
			st->night_gain_baseline = total_gain;
			RSS_INFO("night baseline: gain=%u (day trigger < %u)", total_gain,
				 total_gain * (uint32_t)st->cfg.day_gain_pct / 100);
		}
		return;
	}

	bool want_night, want_day;

	if (st->cfg.trigger == RIC_TRIGGER_LUMA) {
		/*
		 * Hybrid luma+gain algorithm (sensor-independent):
		 *
		 * Day → Night: ae_luma < night_luma.
		 *   No IR LEDs on in day mode, so ae_luma directly reflects
		 *   ambient light. Works identically across all sensors.
		 *
		 * Night → Day: total_gain < day_gain_pct% of night baseline.
		 *   When ambient light returns (dawn, lights on), the ISP
		 *   drops gain because the scene is bright — even with IR
		 *   LEDs still on. This ratio is sensor-independent because
		 *   we compare against the same sensor's own night baseline.
		 *   ae_luma is NOT used here because IR illumination inflates
		 *   it regardless of ambient light level.
		 */
		want_night = (ae_luma < (uint32_t)st->cfg.night_luma) ||
			     (total_gain > (uint32_t)st->cfg.night_gain);

		if (st->night_gain_baseline > 0) {
			uint32_t day_thr =
				st->night_gain_baseline * (uint32_t)st->cfg.day_gain_pct / 100;
			want_day = (total_gain < day_thr);
		} else {
			want_day = false;
		}
	} else if (st->cfg.trigger == RIC_TRIGGER_ADC) {
		/*
		 * ADC mode: read photoresistor via SU_ADC.
		 * Direct ambient light measurement — unaffected by IR LEDs
		 * or camera sensor. No flip-flop, no calibration needed.
		 * High ADC value = bright, low = dark.
		 */
		int adc_val = adc_read(st->cfg.adc_channel);
		if (adc_val < 0) {
			/* ADC read failed — skip this poll */
			return;
		}
		want_night = (adc_val < st->cfg.adc_night);
		want_day = (adc_val > st->cfg.adc_day);
	} else {
		/* Gain mode (legacy): fixed thresholds, sensor-dependent. */
		want_night = (total_gain > (uint32_t)st->cfg.night_threshold);
		want_day = (total_gain < (uint32_t)st->cfg.day_threshold);
	}

	if (st->current_mode == RIC_MODE_DAY) {
		if (want_night) {
			st->night_count++;
			st->day_count = 0;
			if (st->night_count >= st->cfg.hysteresis_sec) {
				RSS_INFO("night detected (luma=%u gain=%u for %ds)", ae_luma,
					 total_gain, st->cfg.hysteresis_sec);
				ric_set_mode(st, RIC_MODE_NIGHT);
			}
		} else {
			st->night_count = 0;
		}
	} else {
		if (want_day) {
			st->day_count++;
			st->night_count = 0;
			if (st->day_count >= st->cfg.hysteresis_sec) {
				uint32_t day_thr = st->night_gain_baseline *
						   (uint32_t)st->cfg.day_gain_pct / 100;
				RSS_INFO("day detected (gain=%u < %u [%d%% of %u] for %ds)",
					 total_gain, day_thr, st->cfg.day_gain_pct,
					 st->night_gain_baseline, st->cfg.hysteresis_sec);
				ric_set_mode(st, RIC_MODE_DAY);
			}
		} else {
			st->day_count = 0;
		}
	}
}

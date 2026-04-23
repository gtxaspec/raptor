/*
 * ric_daynight.c -- IR-cut filter and day/night mode control
 *
 * Supports single GPIO (one pin toggles) and dual GPIO (two pins
 * pulsed for motor-driven IR-cut filters).
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#include "ric.h"

/* ── ADC via kernel device nodes ── */

static int adc_fd = -1;

static int adc_open_channel(int channel)
{
	char path[64];

	snprintf(path, sizeof(path), "/dev/ingenic_adc_aux_%d", channel);
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd >= 0)
		return fd;

	snprintf(path, sizeof(path), "/dev/jz_adc_aux_%d", channel);
	return open(path, O_RDONLY | O_CLOEXEC);
}

bool ric_adc_start(ric_state_t *st)
{
	int channel = st->settings.adc_channel;

	adc_fd = adc_open_channel(channel);
	if (adc_fd < 0) {
		RSS_WARN("ADC: no device for channel %d", channel);
		return false;
	}

	if (ioctl(adc_fd, 0) < 0) {
		RSS_WARN("ADC: enable channel %d failed: %s", channel, strerror(errno));
		close(adc_fd);
		adc_fd = -1;
		return false;
	}

	RSS_DEBUG("ADC: channel %d initialized", channel);
	return true;
}

static int adc_read(int channel)
{
	(void)channel;
	if (adc_fd < 0)
		return -1;
	int value;
	return (read(adc_fd, &value, sizeof(value)) == sizeof(value)) ? value : -1;
}

void ric_adc_cleanup(ric_state_t *st)
{
	if (adc_fd >= 0) {
		ioctl(adc_fd, 1);
		close(adc_fd);
		adc_fd = -1;
	}
	st->adc_initialized = false;
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
		if (write(fd, buf, len) < 0)
			RSS_WARN("gpio export %d: write failed: %s", pin, strerror(errno));
		close(fd);
	}
	/* Set direction to output */
	char path[64];
	snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", pin);
	fd = open(path, O_WRONLY);
	if (fd >= 0) {
		if (write(fd, "out", 3) < 0)
			RSS_WARN("gpio %d direction: write failed: %s", pin, strerror(errno));
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
		if (write(fd, value ? "1" : "0", 1) < 0)
			RSS_WARN("gpio %d set: write failed: %s", pin, strerror(errno));
		close(fd);
	}
}

void ric_gpio_init(ric_state_t *st)
{
	gpio_export(st->settings.gpio_ircut);
	if (st->settings.gpio_ircut2 >= 0)
		gpio_export(st->settings.gpio_ircut2);
	if (st->settings.gpio_irled >= 0)
		gpio_export(st->settings.gpio_irled);
}

/*
 * Set ISP running mode only (day/night) via RVD control socket.
 * Does not toggle GPIO/IR-cut hardware.
 */
void ric_set_isp_mode(ric_mode_t mode)
{
	char resp[128];
	rss_ctrl_send_command(RSS_RUN_DIR "/rvd.sock",
			      mode == RIC_MODE_NIGHT
				      ? "{\"cmd\":\"set-running-mode\",\"value\":\"night\"}"
				      : "{\"cmd\":\"set-running-mode\",\"value\":\"day\"}",
			      resp, sizeof(resp), 2000);
}

static void ric_set_gpio(ric_state_t *st, ric_mode_t mode)
{
	if (mode == RIC_MODE_NIGHT) {
		if (st->settings.gpio_ircut >= 0) {
			if (st->settings.gpio_ircut2 >= 0) {
				gpio_set(st->settings.gpio_ircut, 0);
				gpio_set(st->settings.gpio_ircut2, 1);
				usleep(100000);
				gpio_set(st->settings.gpio_ircut, 0);
				gpio_set(st->settings.gpio_ircut2, 0);
			} else {
				gpio_set(st->settings.gpio_ircut, 0);
			}
		}
		if (st->settings.gpio_irled >= 0)
			gpio_set(st->settings.gpio_irled, 1);
	} else {
		if (st->settings.gpio_ircut >= 0) {
			if (st->settings.gpio_ircut2 >= 0) {
				gpio_set(st->settings.gpio_ircut, 1);
				gpio_set(st->settings.gpio_ircut2, 0);
				usleep(100000);
				gpio_set(st->settings.gpio_ircut, 0);
				gpio_set(st->settings.gpio_ircut2, 0);
			} else {
				gpio_set(st->settings.gpio_ircut, 1);
			}
		}
		if (st->settings.gpio_irled >= 0)
			gpio_set(st->settings.gpio_irled, 0);
	}
}

void ric_set_mode(ric_state_t *st, ric_mode_t mode)
{
	if (mode == st->current_mode)
		return;

	ric_set_gpio(st, mode);
	ric_set_isp_mode(mode);
	RSS_INFO("switched to %s mode", mode == RIC_MODE_NIGHT ? "NIGHT" : "DAY");

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
/* Extract unsigned integer from parsed cJSON object */
static uint32_t json_get_uint(const cJSON *root, const char *key)
{
	const cJSON *item = cJSON_GetObjectItem(root, key);
	return cJSON_IsNumber(item) ? (uint32_t)item->valuedouble : 0;
}

void ric_poll_exposure(ric_state_t *st)
{
	if (st->settings.opmode != RIC_AUTO)
		return;

	/* Query RVD for ISP exposure data via control socket */
	char resp[256];
	int ret = rss_ctrl_send_command(RSS_RUN_DIR "/rvd.sock", "{\"cmd\":\"get-exposure\"}", resp,
					sizeof(resp), 1000);
	if (ret < 0)
		return;

	uint32_t total_gain = 0, ae_luma = 0;
	cJSON *parsed = cJSON_Parse(resp);
	if (!parsed)
		return;
	total_gain = json_get_uint(parsed, "total_gain");
	ae_luma = json_get_uint(parsed, "ae_luma");
	cJSON_Delete(parsed);

	/* Cooldown after mode switch: wait for IR LEDs / ISP to stabilize.
	 * After cooldown in night mode, sample total_gain as baseline for
	 * the auto-calibrating night→day transition. */
	if (st->cooldown_remaining > 0) {
		st->cooldown_remaining--;
		if (st->cooldown_remaining == 0 && st->current_mode == RIC_MODE_NIGHT) {
			st->night_gain_baseline = total_gain;
			RSS_DEBUG("night baseline: gain=%u (day trigger < %u)", total_gain,
				  total_gain * (uint32_t)st->settings.day_gain_pct / 100);
		}
		return;
	}

	bool want_night, want_day;

	if (st->settings.trigger == RIC_TRIGGER_LUMA) {
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
		want_night = (ae_luma < (uint32_t)st->settings.night_luma) ||
			     (total_gain > (uint32_t)st->settings.night_gain);

		if (st->night_gain_baseline > 0) {
			uint32_t day_thr =
				st->night_gain_baseline * (uint32_t)st->settings.day_gain_pct / 100;
			want_day = (total_gain < day_thr);
		} else {
			want_day = false;
		}
	} else if (st->settings.trigger == RIC_TRIGGER_ADC) {
		/*
		 * ADC mode: read photoresistor via SU_ADC.
		 * Direct ambient light measurement — unaffected by IR LEDs
		 * or camera sensor. No flip-flop, no calibration needed.
		 * High ADC value = bright, low = dark.
		 */
		int adc_val = adc_read(st->settings.adc_channel);
		if (adc_val < 0) {
			/* ADC read failed — skip this poll */
			return;
		}
		want_night = (adc_val < st->settings.adc_night);
		want_day = (adc_val > st->settings.adc_day);
	} else {
		/* Gain mode (legacy): fixed thresholds, sensor-dependent. */
		want_night = (total_gain > (uint32_t)st->settings.night_threshold);
		want_day = (total_gain < (uint32_t)st->settings.day_threshold);
	}

	if (st->current_mode == RIC_MODE_DAY) {
		if (want_night) {
			st->night_count++;
			st->day_count = 0;
			if (st->night_count >= st->settings.hysteresis_sec) {
				RSS_DEBUG("night detected (luma=%u gain=%u for %ds)", ae_luma,
					  total_gain, st->settings.hysteresis_sec);
				ric_set_mode(st, RIC_MODE_NIGHT);
			}
		} else {
			st->night_count = 0;
		}
	} else {
		if (want_day) {
			st->day_count++;
			st->night_count = 0;
			if (st->day_count >= st->settings.hysteresis_sec) {
				uint32_t day_thr = st->night_gain_baseline *
						   (uint32_t)st->settings.day_gain_pct / 100;
				RSS_DEBUG("day detected (gain=%u < %u [%d%% of %u] for %ds)",
					  total_gain, day_thr, st->settings.day_gain_pct,
					  st->night_gain_baseline, st->settings.hysteresis_sec);
				ric_set_mode(st, RIC_MODE_DAY);
			}
		} else {
			st->day_count = 0;
		}
	}
}

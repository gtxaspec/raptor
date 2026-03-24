/*
 * ric_daynight.c -- IR-cut filter and day/night mode control
 *
 * Supports single GPIO (one pin toggles) and dual GPIO (two pins
 * pulsed for motor-driven IR-cut filters).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "ric.h"

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

	uint32_t total_gain = 0;
	json_parse_uint(resp, "total_gain", &total_gain);
	uint32_t gain = total_gain;

	if (st->current_mode == RIC_MODE_DAY) {
		/* Check for night transition */
		if (gain > (uint32_t)st->cfg.night_threshold) {
			st->night_count++;
			st->day_count = 0;
			if (st->night_count >= st->cfg.hysteresis_sec) {
				RSS_INFO("night detected (gain=%u > %d for %ds)", gain,
					 st->cfg.night_threshold, st->cfg.hysteresis_sec);
				ric_set_mode(st, RIC_MODE_NIGHT);
			}
		} else {
			st->night_count = 0;
		}
	} else {
		/* Check for day transition */
		if (gain < (uint32_t)st->cfg.day_threshold) {
			st->day_count++;
			st->night_count = 0;
			if (st->day_count >= st->cfg.hysteresis_sec) {
				RSS_INFO("day detected (gain=%u < %d for %ds)", gain,
					 st->cfg.day_threshold, st->cfg.hysteresis_sec);
				ric_set_mode(st, RIC_MODE_DAY);
			}
		} else {
			st->day_count = 0;
		}
	}
}

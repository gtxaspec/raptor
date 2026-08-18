/*
 * ric_json.c -- GPIO pin discovery from a thingino device file
 *
 * Auto-discovers IR-cut and IR LED pins when raptor.conf leaves them
 * at -1. The thingino fleet's standard notation, all of it recognized
 * here, under the top-level "gpio" object:
 *   "ircut": 57                  (single GPIO as integer)
 *   "ircut": "57 58"             (dual-coil pair; pin order defines
 *                                 polarity: first = day, second = night)
 *   "ircut": {"pin": 57, "active_low": true}
 *                                (single inverted pin; the object is
 *                                 the standard's only polarity escape
 *                                 hatch, valid for the LED keys too)
 *   "ircut": {"pin": [52, 53], "delay_us": 100000}
 *                                (dual-coil pair with its hardware
 *                                 pulse width; first = day, second = night)
 *   "ir850": 8 / "ir940": 9      (IR LED GPIOs)
 *   -1 or ""                     (explicitly disabled, silent)
 * "ircut": 999 means the board's filter hangs off the tmi8152
 * motor-driver character device, which ric does not drive yet; that
 * is warned as unsupported, not misread as a pin. The retired o/O
 * drive-level suffix notation is rejected whole rather than
 * half-read: strtol stopping at a suffix used to hold one coil of a
 * dual H-bridge asserted while the other pin was never exported.
 *
 * One optional discovery source: raptor.conf's [ircut] section is
 * authoritative and is consulted first, and the file is absent on any
 * image that is not thingino. A missing file is therefore normal and
 * stays silent.
 *
 * A file that exists but will not parse does warn. Failure here is
 * otherwise invisible -- the pins stay at -1, the >= 0 guards in
 * ric_daynight.c never fire, and the filter is never driven -- and so
 * is indistinguishable from the file being absent unless the log says
 * which one happened. A file that parses and simply carries no "gpio"
 * object is not a fault, and stays debug.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <cJSON.h>
#include <rss_common.h>

#include "ric_json.h"

#define GPIO_PIN_MAX 191
#define TMI8152_DEV  999 /* fleet marker for the motor-driver char device */

/*
 * Bound the read explicitly rather than by the size of whatever
 * buffer is at hand. Nothing bounds this file -- several thingino
 * packages append to it independently -- so the cap is what limits
 * how much gets handed to the parser, and no legitimate config comes
 * close to it.
 */
#define RIC_JSON_MAX (64 * 1024)

static bool valid_gpio(int pin)
{
	return pin >= 0 && pin <= GPIO_PIN_MAX;
}

/*
 * {pin, active_low}: the standard's escape hatch for a single
 * inverted pin. jct types both members properly (pin as a number,
 * active_low as a bool), and the webui's bookkeeping keys riding in
 * the same object (active_on_boot) are not ric's business. A
 * malformed object warns: like everything else here, silence would
 * read as "no pins on this board".
 */
static bool gpio_object(const cJSON *item, const char *key, int *pin, bool *active_low)
{
	if (!cJSON_IsObject(item))
		return false;
	const cJSON *p = cJSON_GetObjectItemCaseSensitive(item, "pin");
	if (!cJSON_IsNumber(p) || !valid_gpio(p->valueint)) {
		RSS_WARN("gpio.%s object carries no usable \"pin\" -- ignored", key);
		return false;
	}
	*pin = p->valueint;
	*active_low = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "active_low"));
	return true;
}

/*
 * "N" or "N M", nothing else. Suffixed tokens and any other trailing
 * decoration reject the whole value: a half-read pair used to hold
 * one coil of a dual H-bridge asserted while the other pin was never
 * exported. "-1" and "" are the explicit-disable spellings and stay
 * silent; other unparseable strings warn.
 */
static bool gpio_pin_pair(const char *s, const char *key, int *pin, int *pin2)
{
	char *endp;
	long v = strtol(s, &endp, 10);
	if (endp == s || v == -1) {
		if (endp == s && *s != '\0')
			RSS_WARN("gpio.%s \"%s\" is not pin notation -- ignored", key, s);
		return false;
	}
	if (v == TMI8152_DEV) {
		RSS_WARN("gpio.%s is the tmi8152 motor-driver device (999), which ric does not "
			 "support yet -- IR-cut switching stays off",
			 key);
		return false;
	}
	if (!valid_gpio((int)v))
		goto bad;
	*pin = (int)v;
	while (*endp == ' ')
		endp++;
	if (*endp == '\0')
		return true;
	const char *second = endp;
	v = strtol(second, &endp, 10);
	if (endp == second || !valid_gpio((int)v))
		goto bad;
	*pin2 = (int)v;
	while (*endp == ' ')
		endp++;
	if (*endp == '\0')
		return true;
bad:
	*pin = -1;
	*pin2 = -1;
	RSS_WARN("gpio.%s \"%s\" is not the standard \"pin\" or \"pin pin\" notation -- "
		 "ignored whole rather than half-read",
		 key, s);
	return false;
}

/* A structured dual-coil description uses one or two bare GPIO
 * numbers. Per-pin polarity is expressed by pair order for pulse-mode
 * filters; accepting nested active_low objects here would pretend ric
 * implements a drive convention it does not. */
static bool gpio_pin_array(const cJSON *item, int *pin, int *pin2)
{
	int n = cJSON_GetArraySize(item);
	if (n < 1 || n > 2)
		goto bad;
	const cJSON *p = cJSON_GetArrayItem(item, 0);
	if (!cJSON_IsNumber(p) || !valid_gpio(p->valueint))
		goto bad;
	*pin = p->valueint;
	if (n == 2) {
		p = cJSON_GetArrayItem(item, 1);
		if (!cJSON_IsNumber(p) || !valid_gpio(p->valueint))
			goto bad;
		*pin2 = p->valueint;
	}
	return true;
bad:
	*pin = -1;
	*pin2 = -1;
	RSS_WARN("gpio.ircut.pin array must contain one or two GPIO numbers -- ignored");
	return false;
}

/* Parse the structured IR-cut form shared with Thingino's generic
 * ircut utility. RIC currently implements pulse drive; rejecting any
 * other named mode is safer than silently applying pulse semantics to
 * level-sequence hardware. */
static bool ircut_object_mode_supported(const cJSON *item)
{
	const cJSON *mode = cJSON_GetObjectItemCaseSensitive(item, "mode");
	if (mode && (!cJSON_IsString(mode) || !mode->valuestring ||
		     strcmp(mode->valuestring, "pulse") != 0)) {
		RSS_WARN("gpio.ircut mode is not pulse -- unsupported by ric");
		return false;
	}
	return true;
}

static bool ircut_object(const cJSON *item, int *pin, int *pin2, bool *active_low)
{
	const cJSON *p = cJSON_GetObjectItemCaseSensitive(item, "pin");
	if (cJSON_IsNumber(p)) {
		if (!valid_gpio(p->valueint))
			goto bad;
		*pin = p->valueint;
		*active_low = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "active_low"));
		return true;
	}
	if (cJSON_IsString(p) && p->valuestring)
		return gpio_pin_pair(p->valuestring, "ircut.pin", pin, pin2);
	if (cJSON_IsArray(p))
		return gpio_pin_array(p, pin, pin2);
bad:
	RSS_WARN("gpio.ircut object carries no usable \"pin\" -- ignored");
	return false;
}

/* thingino.json records actuator timing in microseconds because the
 * shell GPIO utility uses usleep. RIC's public setting is milliseconds;
 * round upward so conversion can never shorten a hardware pulse. */
static void ircut_object_timing(const cJSON *item, ric_config_t *c)
{
	if (c->pulse_ms_explicit)
		return;
	const cJSON *delay = cJSON_GetObjectItemCaseSensitive(item, "delay_us");
	if (!delay)
		return;
	if (!cJSON_IsNumber(delay) || delay->valuedouble < 1 || delay->valuedouble > 1000000) {
		RSS_WARN("gpio.ircut delay_us must be in 1..1000000 -- using %dms", c->pulse_ms);
		return;
	}
	int delay_us = delay->valueint;
	c->pulse_ms = (delay_us + 999) / 1000;
}

/* ir850/ir940: a bare number or the {pin, active_low} object. */
static void gpio_led_pin(const cJSON *gpio, const char *key, int *pin, bool *active_low)
{
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(gpio, key);
	if (cJSON_IsNumber(item)) {
		if (valid_gpio(item->valueint)) {
			*pin = item->valueint;
			*active_low = false;
		} else if (item->valueint > GPIO_PIN_MAX) {
			RSS_WARN("gpio.%s %d is out of range -- ignored", key, item->valueint);
		}
		/* negative = explicitly disabled, silent */
	} else if (cJSON_IsObject(item)) {
		gpio_object(item, key, pin, active_low);
	} else if (cJSON_IsString(item) && item->valuestring && item->valuestring[0] &&
		   strcmp(item->valuestring, "-1") != 0) {
		RSS_WARN("gpio.%s \"%s\": the standard form for an LED pin is a bare number -- "
			 "ignored",
			 key, item->valuestring);
	}
}

void ric_json_gpio_load(ric_config_t *c, const char *path)
{
	if (c->gpio_ircut >= 0 && c->gpio_irled >= 0 && c->gpio_irled2 >= 0 &&
	    c->pulse_ms_explicit)
		return;

	FILE *f = fopen(path, "r");
	if (!f)
		return;

	struct stat sb;
	if (fstat(fileno(f), &sb) != 0 || !S_ISREG(sb.st_mode)) {
		RSS_WARN("%s is not a readable regular file -- GPIO discovery skipped", path);
		fclose(f);
		return;
	}

	if (sb.st_size > RIC_JSON_MAX) {
		RSS_WARN("%s is %lld bytes, over the %d-byte limit -- GPIO discovery skipped", path,
			 (long long)sb.st_size, RIC_JSON_MAX);
		fclose(f);
		return;
	}

	char *buf = malloc((size_t)sb.st_size + 1);
	if (!buf) {
		RSS_WARN("out of memory reading %s -- GPIO discovery skipped", path);
		fclose(f);
		return;
	}

	/*
	 * jct before v1.2.0 rewrote this file in place (its cross-device
	 * rename fallback truncated the target, then refilled it in 4 KiB
	 * chunks), so a concurrent save could shrink the file between the
	 * fstat and this read. jct v1.2.0 replaces the file atomically and
	 * retires that race; the guard stays for images still shipping the
	 * old tool. A torn read would fail the parse anyway -- this only
	 * names the real cause instead of blaming the JSON.
	 */
	size_t n = fread(buf, 1, (size_t)sb.st_size, f);
	fclose(f);
	if (n != (size_t)sb.st_size) {
		RSS_WARN("read %zu of %lld bytes from %s (rewritten mid-read?) -- GPIO discovery "
			 "skipped",
			 n, (long long)sb.st_size, path);
		free(buf);
		return;
	}
	buf[n] = '\0';

	/*
	 * require_null_terminated: trailing bytes after the document are
	 * the one malformation cJSON_Parse accepts silently (it stops at
	 * the first complete value), and a config that only half-parses
	 * should be rejected, not half-honored. Trailing whitespace still
	 * passes. cJSON copies what it keeps, so the buffer goes back
	 * either way.
	 */
	cJSON *root = cJSON_ParseWithOpts(buf, NULL, 1);
	free(buf);
	if (!root) {
		RSS_WARN("%s exists but does not parse as a single JSON document -- GPIO discovery "
			 "skipped, IR-cut and IR LED pins stay unset unless [ircut] provides them",
			 path);
		return;
	}

	cJSON *gpio = cJSON_GetObjectItemCaseSensitive(root, "gpio");
	if (!cJSON_IsObject(gpio)) {
		/* Parsed fine, just has nothing for us: normal, so debug only. */
		RSS_DEBUG("%s has no \"gpio\" object -- using [ircut] config only", path);
		cJSON_Delete(root);
		return;
	}

	cJSON *ircut = cJSON_GetObjectItemCaseSensitive(gpio, "ircut");
	bool ircut_object_ok = !cJSON_IsObject(ircut) || ircut_object_mode_supported(ircut);
	if (cJSON_IsObject(ircut) && ircut_object_ok)
		ircut_object_timing(ircut, c);

	if (c->gpio_ircut < 0) {
		int pin = -1, pin2 = -1;
		bool alow = false;
		if (cJSON_IsNumber(ircut)) {
			if (ircut->valueint == TMI8152_DEV)
				RSS_WARN("gpio.ircut is the tmi8152 motor-driver device (999), "
					 "which ric does not support yet -- IR-cut switching "
					 "stays off");
			else if (valid_gpio(ircut->valueint))
				pin = ircut->valueint;
			else if (ircut->valueint > GPIO_PIN_MAX)
				RSS_WARN("gpio.ircut %d is out of range -- ignored",
					 ircut->valueint);
		} else if (cJSON_IsObject(ircut) && ircut_object_ok) {
			ircut_object(ircut, &pin, &pin2, &alow);
		} else if (cJSON_IsString(ircut) && ircut->valuestring) {
			gpio_pin_pair(ircut->valuestring, "ircut", &pin, &pin2);
		}
		if (pin >= 0) {
			/* The pin's source carries its polarity: a plain form is
			 * active-high by definition, so a stale config flag must
			 * not invert a discovered pin. */
			c->gpio_ircut = pin;
			c->gpio_ircut2 = pin2;
			c->ircut_active_low = alow;
		}
	}

	if (c->gpio_irled < 0)
		gpio_led_pin(gpio, "ir850", &c->gpio_irled, &c->irled_active_low);

	if (c->gpio_irled2 < 0)
		gpio_led_pin(gpio, "ir940", &c->gpio_irled2, &c->irled2_active_low);

	cJSON_Delete(root);

	if (c->gpio_ircut >= 0 || c->gpio_irled >= 0)
		RSS_INFO("GPIOs from %s: ircut=%d ircut2=%d irled=%d irled2=%d pulse=%dms%s%s%s",
			 path,
			 c->gpio_ircut, c->gpio_ircut2, c->gpio_irled, c->gpio_irled2,
			 c->pulse_ms,
			 c->ircut_active_low ? " ircut-active-low" : "",
			 c->irled_active_low ? " irled-active-low" : "",
			 c->irled2_active_low ? " irled2-active-low" : "");
}

/*
 * ric_json.c -- GPIO pin discovery from a thingino device file
 *
 * Auto-discovers IR-cut and IR LED pins when raptor.conf leaves them
 * at -1. Recognized keys under the top-level "gpio" object:
 *   "ircut": "57 58"    (one or two GPIOs, space-separated string)
 *   "ircut": 57         (single GPIO as integer)
 *   "ir850": 8          (IR LED GPIO)
 *   "ir940": 9          (IR LED GPIO, used if ir850 absent)
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

void ric_json_gpio_load(ric_config_t *c, const char *path)
{
	if (c->gpio_ircut >= 0 && c->gpio_irled >= 0 && c->gpio_irled2 >= 0)
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

	if (c->gpio_ircut < 0) {
		cJSON *ircut = cJSON_GetObjectItemCaseSensitive(gpio, "ircut");
		if (cJSON_IsNumber(ircut) && valid_gpio(ircut->valueint)) {
			c->gpio_ircut = ircut->valueint;
		} else if (cJSON_IsString(ircut) && ircut->valuestring) {
			char *endp;
			int val = (int)strtol(ircut->valuestring, &endp, 10);
			if (endp != ircut->valuestring && valid_gpio(val)) {
				c->gpio_ircut = val;
				while (*endp == ' ')
					endp++;
				char *endp2;
				val = (int)strtol(endp, &endp2, 10);
				if (endp2 != endp && valid_gpio(val))
					c->gpio_ircut2 = val;
			}
		}
	}

	if (c->gpio_irled < 0) {
		cJSON *ir850 = cJSON_GetObjectItemCaseSensitive(gpio, "ir850");
		if (cJSON_IsNumber(ir850) && valid_gpio(ir850->valueint))
			c->gpio_irled = ir850->valueint;
	}

	if (c->gpio_irled2 < 0) {
		cJSON *ir940 = cJSON_GetObjectItemCaseSensitive(gpio, "ir940");
		if (cJSON_IsNumber(ir940) && valid_gpio(ir940->valueint))
			c->gpio_irled2 = ir940->valueint;
	}

	cJSON_Delete(root);

	if (c->gpio_ircut >= 0 || c->gpio_irled >= 0)
		RSS_INFO("GPIOs from %s: ircut=%d ircut2=%d irled=%d irled2=%d", path,
			 c->gpio_ircut, c->gpio_ircut2, c->gpio_irled, c->gpio_irled2);
}

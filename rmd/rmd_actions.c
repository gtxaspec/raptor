/*
 * rmd_actions.c -- Motion event actions (GPIO, recording trigger)
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "rmd.h"

/* ── GPIO via sysfs ── */

static int gpio_export(int pin)
{
	int fd = open("/sys/class/gpio/export", O_WRONLY);
	if (fd < 0)
		return -1;
	char buf[16];
	int len = snprintf(buf, sizeof(buf), "%d", pin);
	if (write(fd, buf, len) < 0)
		RSS_WARN("gpio export %d: write failed: %s", pin, strerror(errno));
	close(fd);

	/* Set direction to output */
	char path[64];
	snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", pin);
	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;
	if (write(fd, "out", 3) < 0)
		RSS_WARN("gpio %d direction: write failed: %s", pin, strerror(errno));
	close(fd);
	return 0;
}

static int gpio_set_value(int pin, int value)
{
	char path[64];
	snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin);
	int fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;
	if (write(fd, value ? "1" : "0", 1) < 0)
		RSS_WARN("gpio %d set: write failed: %s", pin, strerror(errno));
	close(fd);
	return 0;
}

void rmd_gpio_init(rmd_ctx_t *ctx)
{
	if (ctx->settings.gpio_pin < 0)
		return;
	if (gpio_export(ctx->settings.gpio_pin) == 0)
		RSS_DEBUG("GPIO %d initialized for motion output", ctx->settings.gpio_pin);
	else
		RSS_WARN("GPIO %d export failed (may already be exported)", ctx->settings.gpio_pin);
}

void rmd_gpio_set(rmd_ctx_t *ctx, bool active)
{
	if (ctx->settings.gpio_pin < 0)
		return;
	gpio_set_value(ctx->settings.gpio_pin, active ? 1 : 0);
	RSS_DEBUG("GPIO %d = %d", ctx->settings.gpio_pin, active ? 1 : 0);
}

/* ── Recording trigger via RMR control socket ── */

int rmd_trigger_recording(rmd_ctx_t *ctx, bool start)
{
	(void)ctx;
	char resp[128];
	const char *cmd = start ? "{\"cmd\":\"start\"}" : "{\"cmd\":\"stop\"}";
	int ret = rss_ctrl_send_command("/var/run/rss/rmr.sock", cmd, resp, sizeof(resp), 2000);
	if (ret < 0)
		RSS_WARN("recording %s command failed (RMR not running?)",
			 start ? "start" : "stop");
	else
		RSS_INFO("recording %s", start ? "started" : "stopped");
	return ret;
}

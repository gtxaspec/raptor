/*
 * rmd_main.c -- Raptor Motion Detection Daemon
 *
 * Queries RVD for IVS motion results via control socket, manages
 * state machine (idle/active/cooldown), triggers recording and GPIO.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>

#include "rmd.h"

static const char *state_names[] = {"idle", "active", "cooldown"};

static void load_config(rmd_ctx_t *ctx)
{
	rss_config_t *cfg = ctx->config;

	ctx->cfg.sensitivity = rss_config_get_int(cfg, "motion", "sensitivity", 3);
	ctx->cfg.cooldown_sec = rss_config_get_int(cfg, "motion", "cooldown_sec", 10);
	ctx->cfg.poll_interval_ms = rss_config_get_int(cfg, "motion", "poll_interval_ms", 500);
	ctx->cfg.record_on_motion = rss_config_get_bool(cfg, "motion", "record", true);
	ctx->cfg.record_post_sec = rss_config_get_int(cfg, "motion", "record_post_sec", 30);
	ctx->cfg.gpio_pin = rss_config_get_int(cfg, "motion", "gpio_pin", -1);
}

/* Query RVD for current motion state */
static bool rmd_poll_motion(void)
{
	char resp[256];
	int ret = rss_ctrl_send_command("/var/run/rss/rvd.sock", "{\"cmd\":\"ivs-status\"}", resp,
					sizeof(resp), 1000);
	if (ret < 0)
		return false;

	/* Parse {"status":"ok","active":true,"motion":true,...} */
	return strstr(resp, "\"motion\":true") != NULL;
}

static void rmd_update_state(rmd_ctx_t *ctx, bool motion)
{
	int64_t now = rss_timestamp_us();
	rmd_state_t prev = ctx->state;

	switch (ctx->state) {
	case RMD_STATE_IDLE:
		if (motion) {
			ctx->state = RMD_STATE_ACTIVE;
			ctx->last_motion_us = now;

			if (ctx->cfg.record_on_motion && !ctx->recording_active) {
				rmd_trigger_recording(ctx, true);
				ctx->recording_active = true;
			}
			rmd_gpio_set(ctx, true);
		}
		break;

	case RMD_STATE_ACTIVE:
		if (motion) {
			ctx->last_motion_us = now;
		} else {
			ctx->state = RMD_STATE_COOLDOWN;
			ctx->cooldown_start_us = now;
		}
		break;

	case RMD_STATE_COOLDOWN:
		if (motion) {
			ctx->state = RMD_STATE_ACTIVE;
			ctx->last_motion_us = now;
		} else {
			int64_t elapsed_sec = (now - ctx->cooldown_start_us) / 1000000;
			int post_sec = ctx->cfg.record_post_sec > ctx->cfg.cooldown_sec
					       ? ctx->cfg.record_post_sec
					       : ctx->cfg.cooldown_sec;
			if (elapsed_sec >= post_sec) {
				ctx->state = RMD_STATE_IDLE;

				if (ctx->recording_active) {
					rmd_trigger_recording(ctx, false);
					ctx->recording_active = false;
				}
				rmd_gpio_set(ctx, false);
			}
		}
		break;
	}

	if (ctx->state != prev)
		RSS_INFO("state: %s -> %s", state_names[prev], state_names[ctx->state]);
}

static int rmd_ctrl_handler(const char *cmd_json, char *resp_buf, int resp_buf_size, void *userdata)
{
	rmd_ctx_t *ctx = userdata;

	int rc = rss_ctrl_handle_common(cmd_json, resp_buf, resp_buf_size, ctx->config, ctx->config_path);
	if (rc >= 0)
		return rc;

	if (strstr(cmd_json, "\"sensitivity\"")) {
		int val;
		if (rss_json_get_int(cmd_json, "value", &val) == 0) {
			ctx->cfg.sensitivity = val;
			/* Relay to RVD */
			char cmd[128], resp[128];
			snprintf(cmd, sizeof(cmd), "{\"cmd\":\"ivs-set-sensitivity\",\"value\":%d}",
				 val);
			rss_ctrl_send_command("/var/run/rss/rvd.sock", cmd, resp, sizeof(resp),
					      1000);
			snprintf(resp_buf, resp_buf_size, "{\"status\":\"ok\",\"sensitivity\":%d}",
				 val);
		} else {
			snprintf(resp_buf, resp_buf_size,
				 "{\"status\":\"error\",\"reason\":\"missing value\"}");
		}
		return (int)strlen(resp_buf);
	}

	/* Default: status */
	snprintf(resp_buf, resp_buf_size,
		 "{\"status\":\"ok\",\"state\":\"%s\",\"recording\":%s,\"sensitivity\":%d}",
		 state_names[ctx->state], ctx->recording_active ? "true" : "false",
		 ctx->cfg.sensitivity);
	return (int)strlen(resp_buf);
}

int main(int argc, char **argv)
{
	rss_daemon_ctx_t dctx;
	int ret = rss_daemon_init(&dctx, "rmd", argc, argv);
	if (ret != 0)
		return ret < 0 ? 1 : 0;
	RSS_BANNER("rmd");

	if (!rss_config_get_bool(dctx.cfg, "motion", "enabled", false)) {
		RSS_INFO("motion detection disabled in config");
		rss_config_free(dctx.cfg);
		rss_daemon_cleanup("rmd");
		return 0;
	}

	rmd_ctx_t ctx = {0};
	ctx.config = dctx.cfg;
	ctx.config_path = dctx.config_path;
	ctx.running = dctx.running;
	ctx.cfg.gpio_pin = -1;
	load_config(&ctx);

	/* Wait for RVD control socket */
	RSS_INFO("waiting for RVD...");
	for (int i = 0; i < 100 && *ctx.running; i++) {
		char resp[128];
		if (rss_ctrl_send_command("/var/run/rss/rvd.sock", "{\"cmd\":\"ivs-status\"}", resp,
					  sizeof(resp), 500) >= 0) {
			if (strstr(resp, "\"active\":true")) {
				RSS_INFO("RVD IVS active");
				break;
			}
		}
		usleep(500000);
	}

	/* GPIO init */
	rmd_gpio_init(&ctx);

	/* Control socket */
	rss_mkdir_p("/var/run/rss");
	ctx.ctrl = rss_ctrl_listen("/var/run/rss/rmd.sock");

	int epoll_fd = epoll_create1(0);
	int ctrl_fd = -1;
	if (ctx.ctrl && epoll_fd >= 0) {
		ctrl_fd = rss_ctrl_get_fd(ctx.ctrl);
		if (ctrl_fd >= 0) {
			struct epoll_event ev = {.events = EPOLLIN, .data.fd = ctrl_fd};
			if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, ctrl_fd, &ev) < 0)
				RSS_ERROR("epoll_ctl add ctrl_fd: %s", strerror(errno));
		}
	}

	RSS_INFO("rmd running (poll=%dms, cooldown=%ds, sensitivity=%d)", ctx.cfg.poll_interval_ms,
		 ctx.cfg.cooldown_sec, ctx.cfg.sensitivity);

	while (*ctx.running) {
		bool motion = rmd_poll_motion();
		rmd_update_state(&ctx, motion);

		if (epoll_fd >= 0) {
			struct epoll_event events[4];
			int n = epoll_wait(epoll_fd, events, 4, ctx.cfg.poll_interval_ms);
			for (int i = 0; i < n; i++) {
				if (events[i].data.fd == ctrl_fd)
					rss_ctrl_accept_and_handle(ctx.ctrl, rmd_ctrl_handler,
								   &ctx);
			}
		} else {
			usleep(ctx.cfg.poll_interval_ms * 1000);
		}
	}

	RSS_INFO("rmd shutting down");

	/* Clean stop */
	if (ctx.recording_active)
		rmd_trigger_recording(&ctx, false);
	rmd_gpio_set(&ctx, false);

	if (ctx.ctrl)
		rss_ctrl_destroy(ctx.ctrl);
	if (epoll_fd >= 0)
		close(epoll_fd);
	rss_config_free(dctx.cfg);
	rss_daemon_cleanup("rmd");

	return 0;
}

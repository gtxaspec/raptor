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
	rss_config_t *cfg = ctx->cfg;

	ctx->settings.sensitivity = rss_config_get_int(cfg, "motion", "sensitivity", 3);
	ctx->settings.skip_frames = rss_config_get_int(cfg, "motion", "skip_frames", 5);
	ctx->settings.cooldown_sec = rss_config_get_int(cfg, "motion", "cooldown_sec", 10);
	ctx->settings.poll_interval_ms = rss_config_get_int(cfg, "motion", "poll_interval_ms", 500);
	ctx->settings.record_on_motion = rss_config_get_bool(cfg, "motion", "record", true);
	ctx->settings.record_post_sec = rss_config_get_int(cfg, "motion", "record_post_sec", 30);
	ctx->settings.gpio_pin = rss_config_get_int(cfg, "motion", "gpio_pin", -1);
}

/* Send configured IVS parameters to RVD */
static void rmd_apply_ivs_config(rmd_ctx_t *ctx)
{
	char cmd[128], resp[128];
	int ok = 0;

	{
		cJSON *j = cJSON_CreateObject();
		cJSON_AddStringToObject(j, "cmd", "ivs-set-sensitivity");
		cJSON_AddNumberToObject(j, "value", ctx->settings.sensitivity);
		cJSON_PrintPreallocated(j, cmd, sizeof(cmd), 0);
		cJSON_Delete(j);
	}
	if (rss_ctrl_send_command("/var/run/rss/rvd.sock", cmd, resp, sizeof(resp), 1000) >= 0)
		ok++;
	else
		RSS_WARN("failed to set RVD sensitivity");

	{
		cJSON *j = cJSON_CreateObject();
		cJSON_AddStringToObject(j, "cmd", "ivs-set-skip-frames");
		cJSON_AddNumberToObject(j, "value", ctx->settings.skip_frames);
		cJSON_PrintPreallocated(j, cmd, sizeof(cmd), 0);
		cJSON_Delete(j);
	}
	if (rss_ctrl_send_command("/var/run/rss/rvd.sock", cmd, resp, sizeof(resp), 1000) >= 0)
		ok++;
	else
		RSS_WARN("failed to set RVD skip_frames");

	if (ok > 0)
		RSS_INFO("IVS config applied (sensitivity=%d, skip_frames=%d)",
			 ctx->settings.sensitivity, ctx->settings.skip_frames);
}

/* Query RVD for current motion state and person count */
static bool rmd_poll_motion(rmd_ctx_t *ctx)
{
	char resp[256];
	int ret = rss_ctrl_send_command("/var/run/rss/rvd.sock", "{\"cmd\":\"ivs-status\"}", resp,
					sizeof(resp), 1000);
	if (ret < 0)
		return false;

	/* Parse person count if available */
	int persons = 0;
	rss_json_get_int(resp, "persons", &persons);
	ctx->person_count = persons > 0 ? persons : 0;

	bool motion = false;
	cJSON *root = cJSON_Parse(resp);
	if (root) {
		motion = cJSON_IsTrue(cJSON_GetObjectItem(root, "motion"));
		cJSON_Delete(root);
	}
	return motion;
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

			if (ctx->settings.record_on_motion && !ctx->recording_active) {
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
			int post_sec = ctx->settings.record_post_sec > ctx->settings.cooldown_sec
					       ? ctx->settings.record_post_sec
					       : ctx->settings.cooldown_sec;
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
		RSS_DEBUG("state: %s -> %s", state_names[prev], state_names[ctx->state]);
}

static int rmd_ctrl_handler(const char *cmd_json, char *resp_buf, int resp_buf_size, void *userdata)
{
	rmd_ctx_t *ctx = userdata;

	int rc = rss_ctrl_handle_common(cmd_json, resp_buf, resp_buf_size, ctx->cfg,
					ctx->config_path);
	if (rc >= 0)
		return rc;

	char cmd[64];
	if (rss_json_get_str(cmd_json, "cmd", cmd, sizeof(cmd)) != 0)
		return rss_ctrl_resp_error(resp_buf, resp_buf_size, "missing cmd");

	if (strcmp(cmd, "sensitivity") == 0) {
		int val;
		if (rss_json_get_int(cmd_json, "value", &val) == 0 && val >= 0) {
			ctx->settings.sensitivity = val;
			char cmd[128], resp[128];
			{
				cJSON *j = cJSON_CreateObject();
				cJSON_AddStringToObject(j, "cmd", "ivs-set-sensitivity");
				cJSON_AddNumberToObject(j, "value", val);
				cJSON_PrintPreallocated(j, cmd, sizeof(cmd), 0);
				cJSON_Delete(j);
			}
			rss_ctrl_send_command("/var/run/rss/rvd.sock", cmd, resp, sizeof(resp),
					      1000);
			cJSON *r = cJSON_CreateObject();
			cJSON_AddStringToObject(r, "status", "ok");
			cJSON_AddNumberToObject(r, "sensitivity", val);
			return rss_ctrl_resp_json(resp_buf, resp_buf_size, r);
		} else {
			return rss_ctrl_resp_error(resp_buf, resp_buf_size,
						   "missing or invalid value");
		}
	}

	if (strcmp(cmd, "skip-frames") == 0) {
		int val;
		if (rss_json_get_int(cmd_json, "value", &val) == 0 && val >= 0) {
			ctx->settings.skip_frames = val;
			char cmd[128], resp[128];
			{
				cJSON *j = cJSON_CreateObject();
				cJSON_AddStringToObject(j, "cmd", "ivs-set-skip-frames");
				cJSON_AddNumberToObject(j, "value", val);
				cJSON_PrintPreallocated(j, cmd, sizeof(cmd), 0);
				cJSON_Delete(j);
			}
			rss_ctrl_send_command("/var/run/rss/rvd.sock", cmd, resp, sizeof(resp),
					      1000);
			cJSON *r = cJSON_CreateObject();
			cJSON_AddStringToObject(r, "status", "ok");
			cJSON_AddNumberToObject(r, "skip_frames", val);
			return rss_ctrl_resp_json(resp_buf, resp_buf_size, r);
		} else {
			return rss_ctrl_resp_error(resp_buf, resp_buf_size,
						   "missing or invalid value");
		}
	}

	/* Default: status */
	cJSON *r = cJSON_CreateObject();
	cJSON_AddStringToObject(r, "status", "ok");
	cJSON_AddStringToObject(r, "state", state_names[ctx->state]);
	cJSON_AddBoolToObject(r, "recording", ctx->recording_active);
	cJSON_AddNumberToObject(r, "sensitivity", ctx->settings.sensitivity);
	cJSON_AddNumberToObject(r, "skip_frames", ctx->settings.skip_frames);
	cJSON_AddNumberToObject(r, "persons", ctx->person_count);
	return rss_ctrl_resp_json(resp_buf, resp_buf_size, r);
}

int main(int argc, char **argv)
{
	rss_daemon_ctx_t dctx;
	int ret = rss_daemon_init(&dctx, "rmd", argc, argv, NULL);
	if (ret != 0)
		return ret < 0 ? 1 : 0;
	rmd_ctx_t ctx = {0};
	ctx.settings.gpio_pin = -1;
	int epoll_fd = -1;

	if (!rss_config_get_bool(dctx.cfg, "motion", "enabled", false)) {
		RSS_INFO("motion detection disabled in config");
		goto cleanup;
	}

	ctx.cfg = dctx.cfg;
	ctx.config_path = dctx.config_path;
	ctx.running = dctx.running;
	load_config(&ctx);

	/* Wait for RVD control socket */
	RSS_INFO("waiting for RVD...");
	for (int i = 0; i < 100 && *ctx.running; i++) {
		char resp[128];
		if (rss_ctrl_send_command("/var/run/rss/rvd.sock", "{\"cmd\":\"ivs-status\"}", resp,
					  sizeof(resp), 500) >= 0) {
			cJSON *root = cJSON_Parse(resp);
			if (root) {
				bool active = cJSON_IsTrue(cJSON_GetObjectItem(root, "active"));
				cJSON_Delete(root);
				if (active) {
					RSS_DEBUG("RVD IVS active");
					break;
				}
			}
		}
		usleep(500000);
	}

	/* Apply motion config to RVD */
	rmd_apply_ivs_config(&ctx);

	/* GPIO init */
	rmd_gpio_init(&ctx);

	/* Control socket */
	rss_mkdir_p("/var/run/rss");
	ctx.ctrl = rss_ctrl_listen("/var/run/rss/rmd.sock");

	epoll_fd = epoll_create1(0);
	int ctrl_fd = -1;
	if (ctx.ctrl && epoll_fd >= 0) {
		ctrl_fd = rss_ctrl_get_fd(ctx.ctrl);
		if (ctrl_fd >= 0) {
			struct epoll_event ev = {.events = EPOLLIN, .data.fd = ctrl_fd};
			if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, ctrl_fd, &ev) < 0)
				RSS_ERROR("epoll_ctl add ctrl_fd: %s", strerror(errno));
		}
	}

	RSS_INFO("rmd running (poll=%dms, cooldown=%ds, sensitivity=%d, skip=%d)",
		 ctx.settings.poll_interval_ms, ctx.settings.cooldown_sec, ctx.settings.sensitivity,
		 ctx.settings.skip_frames);

	while (*ctx.running) {
		bool motion = rmd_poll_motion(&ctx);
		rmd_update_state(&ctx, motion);

		if (epoll_fd >= 0) {
			struct epoll_event events[4];
			int n = epoll_wait(epoll_fd, events, 4, ctx.settings.poll_interval_ms);
			for (int i = 0; i < n; i++) {
				if (events[i].data.fd == ctrl_fd)
					rss_ctrl_accept_and_handle(ctx.ctrl, rmd_ctrl_handler,
								   &ctx);
			}
		} else {
			usleep(ctx.settings.poll_interval_ms * 1000);
		}
	}

	RSS_INFO("rmd shutting down");

cleanup:
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

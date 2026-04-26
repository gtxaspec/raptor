/*
 * rvd_main.c -- Raptor Video Daemon entry point
 *
 * Initializes HAL, sets up the video pipeline, creates SHM rings,
 * and runs the frame acquisition loop until signaled to stop.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <raptor_hal.h>
#include <rss_ipc.h>
#include <rss_common.h>

#include "rvd.h"

/* Bridge HAL logging into the daemon's syslog-aware logger */
static const rss_log_level_t hal_level_map[] = {
	[0] = RSS_LOG_FATAL, [1] = RSS_LOG_ERROR, [2] = RSS_LOG_WARN,
	[3] = RSS_LOG_INFO,  [4] = RSS_LOG_DEBUG,
};

static void hal_log_bridge(int level, const char *file, int line, const char *fmt, ...)
{
	rss_log_level_t lvl = (level >= 0 && level <= 4) ? hal_level_map[level] : RSS_LOG_DEBUG;
	va_list ap;
	va_start(ap, fmt);
	rss_vlog(lvl, file, line, fmt, ap);
	va_end(ap);
}

void rss_post_banner_hook(const char *name)
{
	rss_hal_check_platform(name);
}

int main(int argc, char **argv)
{
	rss_daemon_ctx_t ctx;
	int ret = rss_daemon_init(&ctx, "rvd", argc, argv, NULL);
	if (ret != 0)
		return ret < 0 ? 1 : 0;
	rss_hal_set_log_func(hal_log_bridge);

	/* Initialize state */
	rvd_state_t st = {0};
	st.cfg = ctx.cfg;
	st.config_path = ctx.config_path;

	/* Set up control socket early so clients queue instead of ENOENT */
	rss_mkdir_p(RSS_RUN_DIR);
	st.ctrl = rss_ctrl_listen(RSS_RUN_DIR "/rvd.sock");
	if (!st.ctrl)
		RSS_WARN("control socket failed (non-fatal)");

	/* Set up video pipeline */
	ret = rvd_pipeline_init(&st);
	if (ret != RSS_OK) {
		RSS_FATAL("pipeline init failed: %d", ret);
		goto cleanup;
	}

	RSS_INFO("pipeline initialized, entering frame loop");

	/* Run frame loop */
	rvd_frame_loop(&st, ctx.running);

	RSS_INFO("rvd shutting down");

cleanup:
	if (st.hal_ctx)
		rvd_pipeline_deinit(&st);
	else
		pthread_mutex_destroy(&st.osd_lock);

	if (st.ctrl)
		rss_ctrl_destroy(st.ctrl);

	rss_config_free(ctx.cfg);

	rss_daemon_cleanup("rvd");

	return (ret == RSS_OK) ? 0 : 1;
}

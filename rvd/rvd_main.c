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

int main(int argc, char **argv)
{
	rss_daemon_ctx_t ctx;
	int ret = rss_daemon_init(&ctx, "rvd", argc, argv);
	if (ret != 0)
		return ret < 0 ? 1 : 0;

	/* Initialize state */
	rvd_state_t st = {0};
	st.cfg = ctx.cfg;
	st.config_path = ctx.config_path;

	/* Set up video pipeline */
	ret = rvd_pipeline_init(&st);
	if (ret != RSS_OK) {
		RSS_FATAL("pipeline init failed: %d", ret);
		goto cleanup;
	}

	RSS_INFO("pipeline initialized, entering frame loop");

	/* Set up control socket */
	rss_mkdir_p("/var/run/rss");
	st.ctrl = rss_ctrl_listen("/var/run/rss/rvd.sock");
	if (!st.ctrl)
		RSS_WARN("control socket failed (non-fatal)");

	/* Run frame loop */
	rvd_frame_loop(&st, ctx.running);

	RSS_INFO("rvd shutting down");

cleanup:
	rvd_pipeline_deinit(&st);

	if (st.ctrl)
		rss_ctrl_destroy(st.ctrl);

	rss_config_free(ctx.cfg);

	rss_daemon_cleanup("rvd");

	return (ret == RSS_OK) ? 0 : 1;
}

/*
 * rvd_main.c -- Raptor Video Daemon entry point
 *
 * Initializes HAL, sets up the video pipeline, creates SHM rings,
 * and runs the frame acquisition loop until signaled to stop.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/epoll.h>

#include <raptor_hal.h>
#include <rss_ipc.h>
#include <rss_common.h>

#include "rvd.h"

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"  -c <file>   Config file (default: /etc/raptor.conf)\n"
		"  -f          Run in foreground (don't daemonize)\n"
		"  -d          Debug logging\n"
		"  -h          Show this help\n",
		prog);
}

int main(int argc, char **argv)
{
	const char *config_path = "/etc/raptor.conf";
	bool foreground = false;
	bool debug = false;
	int opt;

	while ((opt = getopt(argc, argv, "c:fdh")) != -1) {
		switch (opt) {
		case 'c': config_path = optarg; break;
		case 'f': foreground = true; break;
		case 'd': debug = true; break;
		case 'h': usage(argv[0]); return 0;
		default:  usage(argv[0]); return 1;
		}
	}

	/* Logging */
	rss_log_init("rvd",
		     debug ? RSS_LOG_DEBUG : RSS_LOG_INFO,
		     foreground ? RSS_LOG_TARGET_STDERR : RSS_LOG_TARGET_SYSLOG,
		     NULL);

	/* Config */
	rss_config_t *cfg = rss_config_load(config_path);
	if (!cfg) {
		RSS_FATAL("failed to load config: %s", config_path);
		return 1;
	}

	/* Daemonize */
	if (!foreground) {
		if (rss_daemonize("rvd", false) < 0) {
			RSS_FATAL("daemonize failed");
			rss_config_free(cfg);
			return 1;
		}
	}

	/* Signals */
	volatile sig_atomic_t *running = rss_signal_init();

	RSS_INFO("rvd starting");

	/* Initialize state */
	rvd_state_t st = {0};
	st.cfg = cfg;
	st.config_path = config_path;

	/* Set up video pipeline */
	int ret = rvd_pipeline_init(&st);
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
	rvd_frame_loop(&st, running);

	RSS_INFO("rvd shutting down");

cleanup:
	rvd_pipeline_deinit(&st);

	if (st.ctrl)
		rss_ctrl_destroy(st.ctrl);

	rss_config_free(cfg);

	if (!foreground)
		rss_daemon_cleanup("rvd");

	return (ret == RSS_OK) ? 0 : 1;
}

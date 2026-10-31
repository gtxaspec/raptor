/*
 * rvd_main.c -- Raptor Video Daemon entry point
 *
 * Initializes HAL, sets up the video pipeline, creates SHM rings,
 * and runs the frame acquisition loop until signaled to stop.
 */

#include <errno.h>
#include <stdarg.h>
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

/*
 * rvd must never be the OOM killer's choice.
 *
 * The kernel picks the process with the most resident memory, and rvd's
 * count includes every shared ring its consumers map, so on a camera it is
 * the pick every time: score 306 on an SSC333, 58 on a Hi3516EV300, the
 * largest in both tables. It is also the one process whose death frees
 * nothing. The pipeline's buffers are the kernel's, not rvd's; a SIGKILL
 * mid-ioctl leaves the ISP driver holding CMA pages it then reports "still
 * in use", the next rvd blocks in the driver's semaphore, and only a reboot
 * clears it. Killing any consumer instead costs a stream for the seconds
 * the init script takes to bring it back.
 *
 * -1000 is OOM_SCORE_ADJ_MIN: never chosen. With nothing killable left the
 * kernel panics, and the panic= in the bootargs reboots the board -- the
 * same recovery the wedge needs, without the hours of not knowing.
 */
static void rvd_oom_shield(void)
{
	FILE *fp = fopen("/proc/self/oom_score_adj", "w");
	if (!fp) {
		RSS_WARN("oom_score_adj: %s -- rvd stays the OOM killer's first pick",
			 strerror(errno));
		return;
	}
	if (fputs("-1000\n", fp) < 0 || fclose(fp) != 0)
		RSS_WARN("oom_score_adj: write failed: %s -- rvd stays the OOM killer's first pick",
			 strerror(errno));
}

/*
 * One line the next out-of-memory report can be read against. On a board
 * with a CMA reserve the number that matters is not MemFree, most of which
 * is CMA the kernel cannot use for its own allocations, but what is left
 * outside the reserve: 24.8 MiB on a 128 MiB Hi3516EV300 with mmz=96M,
 * of which about 2 MiB is free while rvd runs.
 */
static void rvd_log_memory_headroom(void)
{
	FILE *fp = fopen("/proc/meminfo", "r");
	unsigned long total = 0, avail = 0, cma_total = 0, cma_free = 0, mem_free = 0;
	char line[128];

	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp)) {
		unsigned long kb;
		if (sscanf(line, "MemTotal: %lu", &kb) == 1)
			total = kb;
		else if (sscanf(line, "MemFree: %lu", &kb) == 1)
			mem_free = kb;
		else if (sscanf(line, "MemAvailable: %lu", &kb) == 1)
			avail = kb;
		else if (sscanf(line, "CmaTotal: %lu", &kb) == 1)
			cma_total = kb;
		else if (sscanf(line, "CmaFree: %lu", &kb) == 1)
			cma_free = kb;
	}
	fclose(fp);

	if (cma_total)
		RSS_INFO("mem: %lu MiB, %lu MiB of it a CMA reserve; outside the reserve %lu MiB "
			 "with %lu MiB free (%lu MiB reclaimable)",
			 total >> 10, cma_total >> 10, (total - cma_total) >> 10,
			 mem_free > cma_free ? (mem_free - cma_free) >> 10 : 0,
			 avail > mem_free ? (avail - mem_free) >> 10 : 0);
	else
		RSS_INFO("mem: %lu MiB, %lu MiB free, %lu MiB available", total >> 10,
			 mem_free >> 10, avail >> 10);
}

int main(int argc, char **argv)
{
	rss_daemon_ctx_t ctx;
	int ret = rss_daemon_init(&ctx, "rvd", argc, argv, NULL);
	if (ret != 0)
		return ret < 0 ? 1 : 0;
	rss_hal_set_log_func(hal_log_bridge);
	rvd_oom_shield();
	rvd_log_memory_headroom();

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

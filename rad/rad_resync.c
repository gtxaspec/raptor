/*
 * rad_resync.c -- Reporting for bursts of synthetic-audio-clock resyncs.
 */
#include "rad_resync.h"

#include <rss_common.h>

static void rad_resync_report(const rad_resync_log_t *r)
{
	/*
	 * A count of one is the resync rad_resync_note already logged in full,
	 * so a summary would only repeat it. Because the counters run for the
	 * whole episode, that test means the same thing at every report.
	 */
	if (r->count < 2)
		return;

	RSS_WARN("audio clock resynced %u times in %llds: net %+lldms, absolute %lldms", r->count,
		 (long long)((r->last_us - r->open_us) / 1000000), (long long)(r->net_us / 1000),
		 (long long)(r->moved_us / 1000));
}

void rad_resync_note(rad_resync_log_t *r, int64_t now_us, int64_t corr_us)
{
	if (!r->open_us) {
		RSS_WARN("audio clock resync %+lldms (lost samples or stall)",
			 (long long)(corr_us / 1000));
		r->open_us = now_us;
		r->report_us = now_us;
	}

	r->last_us = now_us;
	r->net_us += corr_us;
	r->moved_us += corr_us < 0 ? -corr_us : corr_us;
	r->count++;
}

void rad_resync_tick(rad_resync_log_t *r, int64_t now_us)
{
	if (!r->open_us)
		return;

	if (now_us - r->last_us >= RAD_RESYNC_QUIET_US) {
		rad_resync_report(r);
		*r = (rad_resync_log_t){0};
	} else if (now_us - r->report_us >= RAD_RESYNC_SUMMARY_US) {
		/*
		 * Paced off the last report rather than the episode start, or
		 * every chunk after the first minute reports again.
		 */
		rad_resync_report(r);
		r->report_us = now_us;
	}
}

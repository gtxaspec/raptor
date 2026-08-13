/*
 * rad_resync.h -- Reporting for bursts of synthetic-audio-clock resyncs.
 *
 * One resync is worth a line of its own: it names audio the SDK lost. But a
 * sustained loss episode fires one every time the error climbs back over the
 * threshold, and hundreds of identical warnings evict everything else from a
 * 64 KB syslog ring -- destroying the very context needed to explain them. So
 * the first is logged in full, the rest are counted, and the episode is
 * summarised when it ends and once a minute while it continues.
 *
 * The summary is the better diagnostic anyway: a count and a total say how
 * much audio went missing and how fast, which a stream of identical
 * threshold-sized corrections does not.
 */
#ifndef RAD_RESYNC_H
#define RAD_RESYNC_H

#include <stdint.h>

#define RAD_RESYNC_QUIET_US   5000000  /* no resync for this long ends an episode */
#define RAD_RESYNC_SUMMARY_US 60000000 /* a longer episode reports in as it runs */

/*
 * Counters accumulate for the whole episode and are cleared only when it
 * closes. A periodic report that reset them would leave the next report
 * unable to tell a fresh resync from the one already logged in full.
 */
typedef struct {
	int64_t open_us;   /* when the episode began; 0 when none is open */
	int64_t last_us;   /* the most recent resync */
	int64_t report_us; /* the last periodic report, for pacing the next */
	int64_t net_us;	   /* signed sum: corrections in both directions cancel */
	int64_t moved_us;  /* unsigned sum, which a cancelling net would hide */
	unsigned int count;
} rad_resync_log_t;

/* Record a resync of corr_us. Logs the first of an episode in full. */
void rad_resync_note(rad_resync_log_t *r, int64_t now_us, int64_t corr_us);

/*
 * Advance the clock. Call every chunk, not only on a resync, or an episode
 * that simply stops waits for a resync that never comes to be summarised.
 */
void rad_resync_tick(rad_resync_log_t *r, int64_t now_us);

#endif /* RAD_RESYNC_H */

/*
 * rcd_tsmap.h -- map an external camera's clock onto CLOCK_MONOTONIC
 */

#ifndef RCD_TSMAP_H
#define RCD_TSMAP_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Map a camera's own microsecond clock onto ours. The offset tracks the
 * lowest observed transit delay; it follows a faster sample at once and
 * slews slowly toward a slower one (clock drift), and output timestamps
 * never go backward.
 */
typedef struct {
	bool init;
	int64_t offset;	   /* ours - camera */
	int64_t win_min;   /* lowest delay seen in the current window */
	int64_t win_start; /* local time the window opened */
	int64_t last_out;
	uint64_t last_in;
	uint32_t resets; /* camera clock went backward (it rebooted) */
} rcd_tsmap_t;

int64_t rcd_tsmap(rcd_tsmap_t *m, uint64_t cam_us, int64_t local_us);

/*
 * Source frame counter (ring src_seq) from a camera's frame timestamps.
 * A stamp that does not advance is stale (an EthCam module repeats its
 * start stamp on the first frames of a stream): the frame gets the last
 * one plus a period. Gaps are measured from there, so a missed frame
 * shows up as a jump. A clock that goes back by more than a second means
 * the camera restarted: counted in resets, the sequence moves on.
 */
typedef struct {
	bool init;
	uint64_t last; /* effective timestamp of the last frame */
	uint32_t seq;
	uint32_t resets;
} rcd_seq_t;

/* Returns the frame's sequence number; *eff gets its effective timestamp. */
uint32_t rcd_seq(rcd_seq_t *q, uint64_t cam_us, uint64_t period_us, uint64_t *eff);

#endif

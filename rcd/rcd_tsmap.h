/*
 * rcd_tsmap.h -- map an external camera's clock onto CLOCK_MONOTONIC
 */

#ifndef RCD_TSMAP_H
#define RCD_TSMAP_H

#include <stdatomic.h>
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
	_Atomic uint32_t resets; /* read by the status reply on another thread */
} rcd_seq_t;

/* Returns the frame's sequence number; *eff gets its effective timestamp. */
uint32_t rcd_seq(rcd_seq_t *q, uint64_t cam_us, uint64_t period_us, uint64_t *eff);

/*
 * Spacing between recovery power cycles of the shared camera supply. After
 * each cycle the minimum gap before the next doubles (15 s, 30, 60, 120,
 * then 300 s); ten minutes of streaming without a cycle resets it. A
 * camera that flaps can then cut the supply only every few minutes at
 * worst, while a real outage is retried at most 5 minutes apart. Pure:
 * the caller supplies the clock.
 */
/* LAST is the last doubling; the step after it is the cap. Streaming notes
 * further apart than GAP do not make one stretch. */
#define RCD_BACKOFF_MIN_US    (15 * 1000000LL)
#define RCD_BACKOFF_LAST_US   (120 * 1000000LL)
#define RCD_BACKOFF_MAX_US    (300 * 1000000LL)
#define RCD_BACKOFF_STABLE_US (600 * 1000000LL)
#define RCD_BACKOFF_GAP_US    (30 * 1000000LL)

typedef struct {
	int64_t spacing;      /* gap that follows the next cycle; 0 = not started */
	int64_t next_ok;      /* earliest time of the next cycle */
	int64_t warned_for;   /* next_ok value a refusal was already reported for */
	int64_t stream_since; /* start of the current streaming stretch, 0 = none */
	int64_t last_note;
} rcd_backoff_t;

enum { RCD_BACKOFF_GO, RCD_BACKOFF_REFUSE_WARN, RCD_BACKOFF_REFUSE_QUIET };

/* GO: the cycle may happen now and the spacing advances. Otherwise it is
 * refused, with *wait_us = time until the next cycle is allowed; WARN is
 * returned once per refusal period so a flapping camera cannot flood the
 * log. */
int rcd_backoff_take(rcd_backoff_t *b, int64_t now_us, int64_t *wait_us);

/* Call while any camera is streaming; a stretch of RCD_BACKOFF_STABLE_US
 * without a cycle resets the spacing. */
void rcd_backoff_streaming(rcd_backoff_t *b, int64_t now_us);

#endif

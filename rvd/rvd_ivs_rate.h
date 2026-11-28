/*
 * rvd_ivs_rate.h -- the standalone inference rate limit ([motion] detect_interval_ms)
 */

#ifndef RVD_IVS_RATE_H
#define RVD_IVS_RATE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	int64_t interval_us; /* least time between two inference starts, 0 = every frame */
	int64_t next_us;     /* the earliest start of the next inference */
} rvd_ivs_rate_t;

static inline void rvd_ivs_rate_init(rvd_ivs_rate_t *r, int interval_ms)
{
	r->interval_us = interval_ms > 0 ? (int64_t)interval_ms * 1000 : 0;
	r->next_us = 0;
}

/* True when a frame at now_us should be inferred; the next one is then due a
 * whole interval later. A frame that is not due is released untouched, so the
 * next inference runs on a fresh frame rather than a queued one. */
static inline bool rvd_ivs_rate_due(rvd_ivs_rate_t *r, int64_t now_us)
{
	if (r->interval_us == 0)
		return true;
	if (now_us < r->next_us)
		return false;
	r->next_us = now_us + r->interval_us;
	return true;
}

#endif /* RVD_IVS_RATE_H */

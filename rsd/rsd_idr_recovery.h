/*
 * rsd_idr_recovery.h -- suppress feedback between send stalls and IDR requests
 */

#ifndef RSD_IDR_RECOVERY_H
#define RSD_IDR_RECOVERY_H

#include <stdbool.h>
#include <stdint.h>

#define RSD_IDR_RECOVERY_MIN_INTERVAL_US 1000000

typedef struct {
	int64_t last_event_us;
	bool has_event;
} rsd_idr_recovery_t;

static inline void rsd_idr_recovery_init(rsd_idr_recovery_t *state)
{
	state->last_event_us = 0;
	state->has_event = false;
}

/* A produced keyframe and an explicit request are the same rate-limit event.
 * Counting only requests lets an overflow immediately request another IDR
 * while a natural keyframe is still blocked in the TCP writer. */
static inline void rsd_idr_recovery_note(rsd_idr_recovery_t *state, int64_t now_us)
{
	if (!state->has_event || now_us > state->last_event_us)
		state->last_event_us = now_us;
	state->has_event = true;
}

static inline bool rsd_idr_recovery_request_due(const rsd_idr_recovery_t *state, int64_t now_us)
{
	return !state->has_event ||
	       now_us - state->last_event_us > RSD_IDR_RECOVERY_MIN_INTERVAL_US;
}

#endif /* RSD_IDR_RECOVERY_H */

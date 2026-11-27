/*
 * rsd_slots.h -- who may hold a client slot
 */

#ifndef RSD_SLOTS_H
#define RSD_SLOTS_H

#include <stdbool.h>
#include <stdint.h>

/* A connection that has not yet put one authenticated request through is
 * nobody's session: it gets preauth_us from its accept and no longer. */
static inline bool rsd_preauth_expired(bool authed, int64_t connected_us, int64_t now_us,
				       int64_t preauth_us)
{
	return !authed && now_us - connected_us > preauth_us;
}

/* The connection to give up when a real client finds every slot taken: the
 * oldest one that never authenticated, or -1 when there is none. */
static inline int rsd_pick_evictee(const bool *authed, const int64_t *connected_us, int n)
{
	int pick = -1;

	for (int i = 0; i < n; i++) {
		if (!authed[i] && (pick < 0 || connected_us[i] < connected_us[pick]))
			pick = i;
	}
	return pick;
}

#endif

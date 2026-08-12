/*
 * rsd_media_clock.h -- map a 32-bit RTP timeline to CLOCK_MONOTONIC
 *
 * Header-only so the queue/unit harness can pin the clock behavior without
 * pulling in the RTSP server or Compy.
 */

#ifndef RSD_MEDIA_CLOCK_H
#define RSD_MEDIA_CLOCK_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct {
	uint64_t base_us;
	uint64_t elapsed_ticks;
	uint32_t last_rtp_ts;
	bool initialized;
} rsd_media_clock_t;

static inline void rsd_media_clock_reset(rsd_media_clock_t *clock)
{
	memset(clock, 0, sizeof(*clock));
}

/*
 * Advance the media clock from the actual RTP delta. The unsigned subtraction
 * extends the 32-bit RTP timestamp through wrap. `sample_us` anchors the first
 * timestamp and is also the safe fallback for a malformed zero clock rate.
 */
static inline uint64_t rsd_media_clock_update(rsd_media_clock_t *clock, uint32_t rtp_ts,
					      uint32_t clock_rate, uint64_t sample_us)
{
	if (!clock->initialized) {
		clock->base_us = sample_us;
		clock->elapsed_ticks = 0;
		clock->initialized = true;
	} else {
		clock->elapsed_ticks += (uint32_t)(rtp_ts - clock->last_rtp_ts);
	}
	clock->last_rtp_ts = rtp_ts;

	if (clock_rate == 0)
		return sample_us;

	/* Split quotient and remainder so a multi-year session cannot overflow
	 * elapsed_ticks * 1,000,000 before the division. */
	return clock->base_us + clock->elapsed_ticks / clock_rate * 1000000 +
	       clock->elapsed_ticks % clock_rate * 1000000 / clock_rate;
}

#endif /* RSD_MEDIA_CLOCK_H */

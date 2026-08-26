/*
 * rsd_media_clock.h -- map producer video timestamps to CLOCK_MONOTONIC
 */

#ifndef RSD_MEDIA_CLOCK_H
#define RSD_MEDIA_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

#define RSD_MEDIA_CLOCK_BUCKETS 16

typedef struct {
	int64_t bucket_min_us[RSD_MEDIA_CLOCK_BUCKETS];
	int64_t bucket_start_us;
	int64_t offset_us;
	int64_t err_ewma_us;
	uint8_t bucket;
	bool initialized;
} rsd_media_clock_t;

void rsd_media_clock_init(rsd_media_clock_t *clock);

/*
 * Map one producer timestamp into CLOCK_MONOTONIC. `now_us` is sampled
 * after the frame reaches the ring reader, so now-media contains the
 * clock-domain offset plus non-negative delivery and scheduling delay.
 */
int64_t rsd_media_clock_map(rsd_media_clock_t *clock, int64_t media_us, int64_t now_us);

static inline bool rsd_media_clock_ready(const rsd_media_clock_t *clock)
{
	return clock->initialized;
}

static inline int64_t rsd_media_clock_offset(const rsd_media_clock_t *clock)
{
	return clock->offset_us;
}

#endif /* RSD_MEDIA_CLOCK_H */

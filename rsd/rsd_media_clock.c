/*
 * rsd_media_clock.c -- producer-to-monotonic video clock mapping
 *
 * IMP video timestamps follow CLOCK_MONOTONIC_RAW while sender reports use
 * CLOCK_MONOTONIC. A fixed epoch offset therefore accumulates the frequency
 * correction applied by ntpd. V4L2 timestamps may already be MONOTONIC, so
 * the mapping must discover the relationship instead of assuming a domain.
 *
 * At the ring reader, now-media is the clock-domain offset plus non-negative
 * delivery latency. The minimum observation is the best offset estimate.
 * One minimum per second keeps an eight-second rolling window without a
 * per-frame sample array. The estimate is then followed with the filtered
 * proportional slew used by rad_clock: scheduler jitter cannot step the
 * published timeline, while ordinary clock-frequency error cannot accumulate.
 */

#include "rsd_media_clock.h"

#include <limits.h>

#define RSD_MEDIA_CLOCK_BUCKET_US   1000000
#define RSD_MEDIA_CLOCK_EWMA_DIV    16
#define RSD_MEDIA_CLOCK_GAIN_DIV    16
#define RSD_MEDIA_CLOCK_SLEW_MAX_US 1000

static void clear_buckets(rsd_media_clock_t *clock)
{
	for (int i = 0; i < RSD_MEDIA_CLOCK_BUCKETS; i++)
		clock->bucket_min_us[i] = INT64_MAX;
}

void rsd_media_clock_init(rsd_media_clock_t *clock)
{
	clear_buckets(clock);
	clock->bucket_start_us = 0;
	clock->offset_us = 0;
	clock->err_ewma_us = 0;
	clock->bucket = 0;
	clock->initialized = false;
}

static void advance_window(rsd_media_clock_t *clock, int64_t now_us)
{
	int64_t elapsed = now_us - clock->bucket_start_us;
	int64_t window_us = (int64_t)RSD_MEDIA_CLOCK_BUCKET_US * RSD_MEDIA_CLOCK_BUCKETS;

	if (elapsed < 0 || elapsed >= window_us) {
		clear_buckets(clock);
		clock->bucket = 0;
		clock->bucket_start_us = now_us;
		return;
	}

	while (elapsed >= RSD_MEDIA_CLOCK_BUCKET_US) {
		clock->bucket = (uint8_t)((clock->bucket + 1) % RSD_MEDIA_CLOCK_BUCKETS);
		clock->bucket_min_us[clock->bucket] = INT64_MAX;
		clock->bucket_start_us += RSD_MEDIA_CLOCK_BUCKET_US;
		elapsed -= RSD_MEDIA_CLOCK_BUCKET_US;
	}
}

int64_t rsd_media_clock_map(rsd_media_clock_t *clock, int64_t media_us, int64_t now_us)
{
	int64_t sample_us = now_us - media_us;

	if (!clock->initialized) {
		clock->bucket_start_us = now_us;
		clock->bucket_min_us[0] = sample_us;
		clock->offset_us = sample_us;
		clock->initialized = true;
		return media_us + clock->offset_us;
	}

	advance_window(clock, now_us);
	if (sample_us < clock->bucket_min_us[clock->bucket])
		clock->bucket_min_us[clock->bucket] = sample_us;

	int64_t target_us = INT64_MAX;
	for (int i = 0; i < RSD_MEDIA_CLOCK_BUCKETS; i++) {
		if (clock->bucket_min_us[i] < target_us)
			target_us = clock->bucket_min_us[i];
	}

	if (target_us != INT64_MAX) {
		int64_t err_us = target_us - clock->offset_us;
		clock->err_ewma_us += (err_us - clock->err_ewma_us) / RSD_MEDIA_CLOCK_EWMA_DIV;
		int64_t slew_us = clock->err_ewma_us / RSD_MEDIA_CLOCK_GAIN_DIV;
		if (slew_us > RSD_MEDIA_CLOCK_SLEW_MAX_US)
			slew_us = RSD_MEDIA_CLOCK_SLEW_MAX_US;
		else if (slew_us < -RSD_MEDIA_CLOCK_SLEW_MAX_US)
			slew_us = -RSD_MEDIA_CLOCK_SLEW_MAX_US;
		clock->offset_us += slew_us;
	}

	return media_us + clock->offset_us;
}

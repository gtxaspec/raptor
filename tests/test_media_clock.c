/*
 * test_media_clock.c -- producer video timestamp mapping
 */

#include "greatest.h"
#include "../rsd/rsd_media_clock.h"

#define FRAME_US 40000

static int64_t abs64(int64_t value)
{
	return value < 0 ? -value : value;
}

/* CLOCK_MONOTONIC runs 300 ppm faster than the producer's RAW clock.
 * A frozen epoch loses 54 ms over this run; the tracker must instead
 * reproduce wall-clock rate after its rolling-minimum warmup. */
TEST media_clock_tracks_positive_domain_drift(void)
{
	rsd_media_clock_t clock;
	rsd_media_clock_init(&clock);

	int64_t media_us = 1000000;
	int64_t wall_us = 2000000;
	int64_t warm_err = 0;
	int64_t final_err = 0;

	for (int i = 0; i < 4500; i++) { /* 180 s at 25 fps */
		media_us += FRAME_US;
		wall_us += FRAME_US + 12; /* +300 ppm */
		int64_t latency_us = 2000 + (i % 7) * 37;
		if (i % 113 == 0)
			latency_us += 20000;
		int64_t mapped_us = rsd_media_clock_map(&clock, media_us, wall_us + latency_us);
		if (i == 1499)
			warm_err = mapped_us - wall_us;
		if (i == 4499)
			final_err = mapped_us - wall_us;
	}

	ASSERT(abs64(final_err) < 5000);
	ASSERT(abs64(final_err - warm_err) < 2000);
	PASS();
}

/* V4L2 timestamps are already CLOCK_MONOTONIC. Reader latency is a
 * non-negative contaminant, so isolated stalls must not move the mapping. */
TEST media_clock_rejects_reader_latency_spikes(void)
{
	rsd_media_clock_t clock;
	rsd_media_clock_init(&clock);

	int64_t media_us = 1000000;
	int64_t previous_offset = 0;
	for (int i = 0; i < 1500; i++) {
		media_us += FRAME_US;
		int64_t latency_us = 1800 + (i % 5) * 20;
		if (i > 0 && i % 97 == 0)
			latency_us += 100000;
		rsd_media_clock_map(&clock, media_us, media_us + latency_us);
		int64_t offset = rsd_media_clock_offset(&clock);
		if (i > 0)
			ASSERT(abs64(offset - previous_offset) <= 1000);
		previous_offset = offset;
	}

	ASSERT(rsd_media_clock_offset(&clock) >= 1750);
	ASSERT(rsd_media_clock_offset(&clock) <= 2500);
	PASS();
}

/* A rising domain offset must age the old minimum out of the window.
 * The correction may move only by the advertised per-frame authority. */
TEST media_clock_ages_minimum_with_bounded_slew(void)
{
	rsd_media_clock_t clock;
	rsd_media_clock_init(&clock);

	int64_t media_us = 1000000;
	int64_t previous_offset = 0;
	for (int i = 0; i < 750; i++) {
		media_us += FRAME_US;
		int64_t domain_offset = i < 250 ? 1000 : 21000;
		rsd_media_clock_map(&clock, media_us, media_us + domain_offset + 1000);
		int64_t offset = rsd_media_clock_offset(&clock);
		if (i > 0)
			ASSERT(abs64(offset - previous_offset) <= 1000);
		previous_offset = offset;
	}

	ASSERT(rsd_media_clock_offset(&clock) > 18000);
	PASS();
}

SUITE(media_clock_suite)
{
	RUN_TEST(media_clock_tracks_positive_domain_drift);
	RUN_TEST(media_clock_rejects_reader_latency_spikes);
	RUN_TEST(media_clock_ages_minimum_with_bounded_slew);
}

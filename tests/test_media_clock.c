/*
 * test_media_clock.c -- producer video timestamp mapping
 */

#include "greatest.h"
#include <rss_media_clock.h>

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
	rss_media_clock_t clock;
	rss_media_clock_init(&clock);

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
		int64_t mapped_us = rss_media_clock_map(&clock, media_us, wall_us + latency_us);
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
	rss_media_clock_t clock;
	rss_media_clock_init(&clock);

	int64_t media_us = 1000000;
	int64_t previous_offset = 0;
	for (int i = 0; i < 1500; i++) {
		media_us += FRAME_US;
		int64_t latency_us = 1800 + (i % 5) * 20;
		if (i > 0 && i % 97 == 0)
			latency_us += 100000;
		rss_media_clock_map(&clock, media_us, media_us + latency_us);
		int64_t offset = rss_media_clock_offset(&clock);
		if (i > 0)
			ASSERT(abs64(offset - previous_offset) <= 1000);
		previous_offset = offset;
	}

	ASSERT(rss_media_clock_offset(&clock) >= 1750);
	ASSERT(rss_media_clock_offset(&clock) <= 2500);
	PASS();
}

/* A rising domain offset must age the old minimum out of the window.
 * The correction may move only by the advertised per-frame authority. */
TEST media_clock_ages_minimum_with_bounded_slew(void)
{
	rss_media_clock_t clock;
	rss_media_clock_init(&clock);

	int64_t media_us = 1000000;
	int64_t previous_offset = 0;
	for (int i = 0; i < 750; i++) {
		media_us += FRAME_US;
		int64_t domain_offset = i < 250 ? 1000 : 21000;
		rss_media_clock_map(&clock, media_us, media_us + domain_offset + 1000);
		int64_t offset = rss_media_clock_offset(&clock);
		if (i > 0)
			ASSERT(abs64(offset - previous_offset) <= 1000);
		previous_offset = offset;
	}

	ASSERT(rss_media_clock_offset(&clock) > 18000);
	PASS();
}

/* A sub-stream frame normally publishes behind the main-stream encode of
 * the same capture, and a few frames per minute skip that wait. Those
 * isolated fast deliveries are the window's true minimum but a terrible
 * estimator: following them flips the mapping by the encode time every
 * time one enters or leaves the window. Measured on a T31: 29 ms, every
 * 0.4 to 31 s. The mapping must sit on the floor every second reaches. */
TEST media_clock_ignores_rare_fast_frames(void)
{
	rss_media_clock_t clock;
	rss_media_clock_init(&clock);

	int64_t media_us = 1000000;
	int64_t previous_offset = 0;
	int64_t lo = INT64_MAX, hi = INT64_MIN;
	static const int fast_at[] = {320,  427,  592,	624,  680,  1084, 1085, 1660, 1864, 2340,
				      2436, 3328, 3454, 3611, 3612, 3706, 3770, 3782, 3783, 4028};
	int next_fast = 0;
	for (int i = 0; i < 4300; i++) { /* 172 s at 25 fps */
		media_us += FRAME_US;
		int64_t latency_us = 30000 + (i % 9) * 300;
		if (next_fast < (int)(sizeof(fast_at) / sizeof(fast_at[0])) &&
		    i == fast_at[next_fast]) {
			latency_us = 1000;
			next_fast++;
		}
		rss_media_clock_map(&clock, media_us, media_us + latency_us);
		int64_t offset = rss_media_clock_offset(&clock);
		if (i > 0)
			ASSERT(abs64(offset - previous_offset) <= 1000);
		previous_offset = offset;
		if (i >= 500) {
			if (offset < lo)
				lo = offset;
			if (offset > hi)
				hi = offset;
		}
	}

	/* On the every-second floor, and never more than a frame-jitter
	 * away from it: the 29 ms fast path never reached the estimate. */
	ASSERT(lo >= 27000);
	ASSERT(hi - lo < 4000);
	PASS();
}

SUITE(media_clock_suite)
{
	RUN_TEST(media_clock_tracks_positive_domain_drift);
	RUN_TEST(media_clock_rejects_reader_latency_spikes);
	RUN_TEST(media_clock_ages_minimum_with_bounded_slew);
	RUN_TEST(media_clock_ignores_rare_fast_frames);
}

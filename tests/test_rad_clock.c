/*
 * test_rad_clock.c -- the synthetic audio clock's control law
 *
 * The behaviors pinned here are field lessons: the drain and pacing
 * gates came from T23 stall recovery, the asymmetric resync thresholds
 * from real salvage bursts, and the no-sawtooth bound from a T31 where
 * the old deadband-and-nudge law wandered ~20ms and walked back in
 * steps that every RTCP sender report faithfully reported as timeline
 * corrections.
 */

#include "greatest.h"
#include "../rad/rad_clock.h"

#define CHUNK_SAMPLES 320
#define RATE	      16000
#define CHUNK_US      20000

/* Crystal error: wall runs fast against the sample count. The clock
 * must track it closely and smoothly -- steady error under 8ms (the
 * old law is only bounded by its 20ms deadband, and fails this), and
 * no stamp-to-stamp step beyond the slew authority. */
TEST rad_clock_tracks_crystal_drift_without_sawtooth(void)
{
	rad_clock_t c;
	int64_t now = 1000000;
	rad_clock_init(&c, now);

	int64_t prev = 0;
	int64_t max_abs_err = 0;
	bool have_prev = false;
	for (int i = 0; i < 3000; i++) { /* 60s at 500ppm */
		now += CHUNK_US + 10;
		int64_t ts = rad_clock_stamp(&c, CHUNK_SAMPLES, RATE, now);
		ASSERT_EQ_FMT((int64_t)0, c.resync_us, "%lld");
		if (have_prev) {
			int64_t delta = ts - prev;
			ASSERT(delta >= CHUNK_US - 1001);
			ASSERT(delta <= CHUNK_US + 1001);
		}
		prev = ts;
		have_prev = true;
		int64_t err = now - c.ts;
		if (err < 0)
			err = -err;
		if (err > max_abs_err)
			max_abs_err = err;
	}
	ASSERT(max_abs_err < 8000);
	PASS();
}

/* Chunk-arrival jitter is not drift: alternating early/late reads must
 * not excite the slew into oscillation. */
TEST rad_clock_ignores_arrival_jitter(void)
{
	rad_clock_t c;
	int64_t now = 1000000;
	rad_clock_init(&c, now);

	int64_t worst_slew = 0;
	int64_t prev_ts = 0;
	bool have_prev = false;
	for (int i = 0; i < 500; i++) {
		now += CHUNK_US + ((i & 1) ? 2000 : -2000);
		int64_t ts = rad_clock_stamp(&c, CHUNK_SAMPLES, RATE, now);
		if (have_prev) {
			int64_t s = (ts - prev_ts) - CHUNK_US;
			if (s < 0)
				s = -s;
			if (s > worst_slew)
				worst_slew = s;
		}
		prev_ts = ts;
		have_prev = true;
	}
	ASSERT(worst_slew <= 300);
	PASS();
}

/* A stall loses samples for good. The first live-paced read after it
 * carries the residual error, and the resync must insert that gap. */
TEST rad_clock_resyncs_after_stall(void)
{
	rad_clock_t c;
	int64_t now = 1000000;
	rad_clock_init(&c, now);

	for (int i = 0; i < 40; i++) {
		now += CHUNK_US;
		rad_clock_stamp(&c, CHUNK_SAMPLES, RATE, now);
	}
	/* Stall: 500ms passes, the stale buffered chunk returns with a
	 * long gap -- NOT live-paced, must not resync on old audio. */
	now += 500000;
	rad_clock_stamp(&c, CHUNK_SAMPLES, RATE, now);
	ASSERT_EQ_FMT((int64_t)0, c.resync_us, "%lld");
	/* Next read is live-paced; the loss is real now. */
	now += CHUNK_US;
	rad_clock_stamp(&c, CHUNK_SAMPLES, RATE, now);
	ASSERT(c.resync_us > 400000);
	int64_t err = now - c.ts;
	ASSERT(err > -CHUNK_US * 2 && err < CHUNK_US * 2);
	PASS();
}

/* Startup drain: the SDK hands out ~400ms of buffered chunks
 * back-to-back. The clock runs ahead of wall; that is salvage, not a
 * fault -- no backward resync, timeline stays monotone. */
TEST rad_clock_tolerates_drain_burst(void)
{
	rad_clock_t c;
	int64_t now = 1000000;
	rad_clock_init(&c, now);

	int64_t prev_ts = -1;
	for (int i = 0; i < 20; i++) {
		now += 1000; /* instant reads: 1ms apart */
		int64_t ts = rad_clock_stamp(&c, CHUNK_SAMPLES, RATE, now);
		ASSERT_EQ_FMT((int64_t)0, c.resync_us, "%lld");
		ASSERT(ts > prev_ts);
		prev_ts = ts;
	}
	PASS();
}

/* More than a second ahead on live-paced reads is not salvage. */
TEST rad_clock_resyncs_when_impossibly_ahead(void)
{
	rad_clock_t c;
	int64_t now = 1000000;
	rad_clock_init(&c, now);

	/* Force the clock far ahead: many instant chunks. */
	for (int i = 0; i < 60; i++) {
		now += 500;
		rad_clock_stamp(&c, CHUNK_SAMPLES, RATE, now);
	}
	/* Live-paced read with the clock >1s ahead of wall. */
	now += CHUNK_US;
	rad_clock_stamp(&c, CHUNK_SAMPLES, RATE, now);
	ASSERT(c.resync_us < 0);
	PASS();
}

SUITE(rad_clock_suite)
{
	RUN_TEST(rad_clock_tracks_crystal_drift_without_sawtooth);
	RUN_TEST(rad_clock_ignores_arrival_jitter);
	RUN_TEST(rad_clock_resyncs_after_stall);
	RUN_TEST(rad_clock_tolerates_drain_burst);
	RUN_TEST(rad_clock_resyncs_when_impossibly_ahead);
}

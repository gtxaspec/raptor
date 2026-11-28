/*
 * test_ivs_rate.c -- [motion] detect_interval_ms: the standalone inference rate limit
 */

#include "greatest.h"
#include "../rvd/rvd_ivs_rate.h"

TEST ivs_rate_zero_infers_every_frame(void)
{
	rvd_ivs_rate_t r;
	rvd_ivs_rate_init(&r, 0);

	for (int64_t t = 0; t < 1000000; t += 40000)
		ASSERT(rvd_ivs_rate_due(&r, t));
	PASS();
}

TEST ivs_rate_negative_is_every_frame(void)
{
	rvd_ivs_rate_t r;
	rvd_ivs_rate_init(&r, -5);

	ASSERT_EQ(0, r.interval_us);
	ASSERT(rvd_ivs_rate_due(&r, 1000));
	ASSERT(rvd_ivs_rate_due(&r, 1001));
	PASS();
}

TEST ivs_rate_first_frame_is_due(void)
{
	rvd_ivs_rate_t r;
	rvd_ivs_rate_init(&r, 500);

	ASSERT(rvd_ivs_rate_due(&r, 123456789));
	PASS();
}

TEST ivs_rate_holds_for_the_interval(void)
{
	rvd_ivs_rate_t r;
	rvd_ivs_rate_init(&r, 500);

	ASSERT(rvd_ivs_rate_due(&r, 10000000));
	ASSERT_FALSE(rvd_ivs_rate_due(&r, 10040000));
	ASSERT_FALSE(rvd_ivs_rate_due(&r, 10499999));
	ASSERT(rvd_ivs_rate_due(&r, 10500000));
	ASSERT_FALSE(rvd_ivs_rate_due(&r, 10500001));
	PASS();
}

/* The interval runs from the frame that was inferred, not from when the
 * previous interval ran out: a late frame does not bring the next one forward. */
TEST ivs_rate_counts_from_the_inferred_frame(void)
{
	rvd_ivs_rate_t r;
	rvd_ivs_rate_init(&r, 500);

	ASSERT(rvd_ivs_rate_due(&r, 0));
	ASSERT(rvd_ivs_rate_due(&r, 900000));
	ASSERT_FALSE(rvd_ivs_rate_due(&r, 1000000));
	ASSERT_FALSE(rvd_ivs_rate_due(&r, 1399999));
	ASSERT(rvd_ivs_rate_due(&r, 1400000));
	PASS();
}

/* 25 fps frames, one inference a second: 25 frames apart, 10 in 10 s. */
TEST ivs_rate_at_25_fps(void)
{
	rvd_ivs_rate_t r;
	rvd_ivs_rate_init(&r, 1000);
	int n = 0;

	for (int i = 0; i < 250; i++)
		n += rvd_ivs_rate_due(&r, (int64_t)i * 40000);
	ASSERT_EQ(10, n);
	PASS();
}

/* An hour is 3.6e9 us, past a 32-bit int. */
TEST ivs_rate_large_interval(void)
{
	rvd_ivs_rate_t r;
	rvd_ivs_rate_init(&r, 3600000);

	ASSERT(r.interval_us == (int64_t)3600000 * 1000);
	ASSERT(rvd_ivs_rate_due(&r, 0));
	ASSERT_FALSE(rvd_ivs_rate_due(&r, (int64_t)3600000 * 1000 - 1));
	ASSERT(rvd_ivs_rate_due(&r, (int64_t)3600000 * 1000));
	PASS();
}

SUITE(ivs_rate_suite)
{
	RUN_TEST(ivs_rate_zero_infers_every_frame);
	RUN_TEST(ivs_rate_negative_is_every_frame);
	RUN_TEST(ivs_rate_first_frame_is_due);
	RUN_TEST(ivs_rate_holds_for_the_interval);
	RUN_TEST(ivs_rate_counts_from_the_inferred_frame);
	RUN_TEST(ivs_rate_at_25_fps);
	RUN_TEST(ivs_rate_large_interval);
}

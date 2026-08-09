#include "greatest.h"
#include "../rmr/rmr_timelapse.h"

#define S(n) ((int64_t)(n) * 1000000)

TEST tl_first_tick_arms_immediately(void)
{
	rmr_tl_t t;
	rmr_tl_init(&t, 10, 30, 0);
	ASSERT_FALSE(t.want_sample);
	rmr_tl_tick(&t, S(100));
	ASSERT(t.want_sample);
	PASS();
}

TEST tl_cadence(void)
{
	rmr_tl_t t;
	rmr_tl_init(&t, 10, 30, 0);
	rmr_tl_tick(&t, S(100));
	ASSERT(rmr_tl_take(&t, 1));
	rmr_tl_tick(&t, S(105));
	ASSERT_FALSE(t.want_sample);
	rmr_tl_tick(&t, S(109));
	ASSERT_FALSE(t.want_sample);
	rmr_tl_tick(&t, S(110));
	ASSERT(t.want_sample);
	ASSERT(rmr_tl_take(&t, 2));
	rmr_tl_tick(&t, S(120));
	ASSERT(t.want_sample);
	PASS();
}

TEST tl_no_catchup_burst(void)
{
	rmr_tl_t t;
	rmr_tl_init(&t, 10, 30, 0);
	rmr_tl_tick(&t, S(100));
	ASSERT(rmr_tl_take(&t, 1));
	/* 35s stall: exactly one arm, rescheduled from now */
	rmr_tl_tick(&t, S(145));
	ASSERT(t.want_sample);
	ASSERT(rmr_tl_take(&t, 2));
	rmr_tl_tick(&t, S(146));
	ASSERT_FALSE(t.want_sample);
	rmr_tl_tick(&t, S(154));
	ASSERT_FALSE(t.want_sample);
	rmr_tl_tick(&t, S(155));
	ASSERT(t.want_sample);
	PASS();
}

TEST tl_take_consumes_and_guards_seq(void)
{
	rmr_tl_t t;
	rmr_tl_init(&t, 10, 30, 0);
	rmr_tl_tick(&t, S(100));
	ASSERT(rmr_tl_take(&t, 7));
	/* consumed: same tick cannot take twice */
	ASSERT_FALSE(rmr_tl_take(&t, 8));
	/* next tick armed, but the same ring seq never satisfies it */
	rmr_tl_tick(&t, S(110));
	ASSERT_FALSE(rmr_tl_take(&t, 7));
	ASSERT(t.want_sample);
	ASSERT(rmr_tl_take(&t, 8));
	PASS();
}

TEST tl_take_without_tick_false(void)
{
	rmr_tl_t t;
	rmr_tl_init(&t, 10, 30, 0);
	ASSERT_FALSE(rmr_tl_take(&t, 1));
	PASS();
}

TEST tl_force_arms(void)
{
	rmr_tl_t t;
	rmr_tl_init(&t, 10, 30, 0);
	rmr_tl_tick(&t, S(100));
	ASSERT(rmr_tl_take(&t, 1));
	ASSERT_FALSE(t.want_sample);
	rmr_tl_force(&t);
	ASSERT(rmr_tl_take(&t, 2));
	PASS();
}

TEST tl_clamps(void)
{
	rmr_tl_t t;
	rmr_tl_init(&t, 0, 0, 5);
	ASSERT_EQ(S(RMR_TL_MIN_INTERVAL_SEC), t.interval_us);
	ASSERT_EQ(RMR_TL_MIN_FPS, t.playback_fps);
	ASSERT_EQ(RMR_TL_MIN_FILE_FRAMES, t.file_frames);
	rmr_tl_init(&t, -3, 500, 0);
	ASSERT_EQ(S(RMR_TL_MIN_INTERVAL_SEC), t.interval_us);
	ASSERT_EQ(RMR_TL_MAX_FPS, t.playback_fps);
	ASSERT_EQ(0, t.file_frames);
	rmr_tl_set_interval(&t, 1);
	ASSERT_EQ(S(RMR_TL_MIN_INTERVAL_SEC), t.interval_us);
	PASS();
}

TEST tl_dts_exact_spacing(void)
{
	rmr_tl_t t;
	rmr_tl_init(&t, 10, 30, 0);
	rmr_tl_file_opened(&t, 20260808);
	ASSERT_EQ(0, rmr_tl_sample_dts90(&t));
	ASSERT_EQ(3000, rmr_tl_sample_dts90(&t));
	ASSERT_EQ(6000, rmr_tl_sample_dts90(&t));
	for (int i = 3; i < 30; i++)
		rmr_tl_sample_dts90(&t);
	/* frame 30 lands on exactly one second */
	ASSERT_EQ(90000, rmr_tl_sample_dts90(&t));

	/* a rate that does not divide 90000 must not accumulate error:
	 * frame 7 at 7 fps is exactly one second */
	rmr_tl_init(&t, 10, 7, 0);
	rmr_tl_file_opened(&t, 20260808);
	int64_t dts = 0;
	for (int i = 0; i <= 7; i++)
		dts = rmr_tl_sample_dts90(&t);
	ASSERT_EQ(90000, dts);
	PASS();
}

TEST tl_rotate_daily(void)
{
	rmr_tl_t t;
	rmr_tl_init(&t, 10, 30, 0);
	/* no file yet: first sample always rotates */
	ASSERT(rmr_tl_needs_rotate(&t, 20260808));
	rmr_tl_file_opened(&t, 20260808);
	ASSERT_FALSE(rmr_tl_needs_rotate(&t, 20260808));
	ASSERT(rmr_tl_needs_rotate(&t, 20260809));
	/* backward step to an earlier date also rotates */
	ASSERT(rmr_tl_needs_rotate(&t, 20260807));
	PASS();
}

TEST tl_rotate_by_frames(void)
{
	rmr_tl_t t;
	rmr_tl_init(&t, 10, 30, 60);
	ASSERT(rmr_tl_needs_rotate(&t, 20260808));
	rmr_tl_file_opened(&t, 20260808);
	for (int i = 0; i < 59; i++) {
		rmr_tl_sample_dts90(&t);
		ASSERT_FALSE(rmr_tl_needs_rotate(&t, 20260808));
	}
	rmr_tl_sample_dts90(&t);
	ASSERT(rmr_tl_needs_rotate(&t, 20260808));
	/* frames mode ignores the date: a file may span midnight */
	rmr_tl_file_opened(&t, 20260808);
	rmr_tl_sample_dts90(&t);
	ASSERT_FALSE(rmr_tl_needs_rotate(&t, 20260809));
	PASS();
}

TEST tl_fps_change_rotates(void)
{
	rmr_tl_t t;
	rmr_tl_init(&t, 10, 30, 0);
	rmr_tl_file_opened(&t, 20260808);
	rmr_tl_sample_dts90(&t);
	rmr_tl_sample_dts90(&t);
	ASSERT_FALSE(rmr_tl_set_playback_fps(&t, 30));
	ASSERT(rmr_tl_set_playback_fps(&t, 15));
	/* caller rotates; new file restarts the timeline at the new rate */
	rmr_tl_file_opened(&t, 20260808);
	ASSERT_EQ(0, rmr_tl_sample_dts90(&t));
	ASSERT_EQ(6000, rmr_tl_sample_dts90(&t));
	PASS();
}

TEST tl_ring_reset_forgets_seq_guard(void)
{
	rmr_tl_t t;
	rmr_tl_init(&t, 10, 30, 0);
	rmr_tl_tick(&t, S(100));
	ASSERT(rmr_tl_take(&t, 170));
	/* Producer restarted: the new ring's seqs start over. Without a
	 * reset every new frame reads as already sampled and the
	 * timelapse silently stops -- found on hardware, not x86. */
	rmr_tl_tick(&t, S(110));
	ASSERT_FALSE(rmr_tl_take(&t, 1));
	rmr_tl_ring_reset(&t);
	ASSERT(rmr_tl_take(&t, 1));
	PASS();
}

SUITE(timelapse_suite)
{
	RUN_TEST(tl_first_tick_arms_immediately);
	RUN_TEST(tl_cadence);
	RUN_TEST(tl_no_catchup_burst);
	RUN_TEST(tl_take_consumes_and_guards_seq);
	RUN_TEST(tl_take_without_tick_false);
	RUN_TEST(tl_force_arms);
	RUN_TEST(tl_clamps);
	RUN_TEST(tl_dts_exact_spacing);
	RUN_TEST(tl_rotate_daily);
	RUN_TEST(tl_rotate_by_frames);
	RUN_TEST(tl_fps_change_rotates);
	RUN_TEST(tl_ring_reset_forgets_seq_guard);
}

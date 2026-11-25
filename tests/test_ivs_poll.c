/*
 * test_ivs_poll.c -- what the IVS thread runs after rvd_ivs_start()
 */

#include "greatest.h"
#include "../rvd/rvd_ivs_poll.h"

static int dummy_handle;

TEST ivs_poll_algo_created_polls(void)
{
	ASSERT_EQ(RVD_IVS_THREAD_POLL, rvd_ivs_thread_mode(false, &dummy_handle));
	PASS();
}

/* the case that spun a core: the JZDL model or the algorithm interface failed
 * to load, the group is still bound, no channel was created */
TEST ivs_poll_no_algo_runs_nothing(void)
{
	ASSERT_EQ(RVD_IVS_THREAD_NONE, rvd_ivs_thread_mode(false, NULL));
	PASS();
}

TEST ivs_poll_jzdl_runs_jzdl(void)
{
	ASSERT_EQ(RVD_IVS_THREAD_JZDL, rvd_ivs_thread_mode(true, NULL));
	PASS();
}

/* JZDL mode creates no IVS algorithm; a stale handle must not turn it into a poll */
TEST ivs_poll_jzdl_wins_over_a_handle(void)
{
	ASSERT_EQ(RVD_IVS_THREAD_JZDL, rvd_ivs_thread_mode(true, &dummy_handle));
	PASS();
}

SUITE(ivs_poll_suite)
{
	RUN_TEST(ivs_poll_algo_created_polls);
	RUN_TEST(ivs_poll_no_algo_runs_nothing);
	RUN_TEST(ivs_poll_jzdl_runs_jzdl);
	RUN_TEST(ivs_poll_jzdl_wins_over_a_handle);
}

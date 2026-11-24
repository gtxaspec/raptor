/*
 * test_frame_result.c -- how the frame loop reads enc_get_frame() results
 */

#include <errno.h>

#include "greatest.h"
#include "../rvd/rvd_frame_result.h"

/* Feed ret n times; count how often each verdict came back. */
static void feed(int ret, int n, int *empty, int *stalls, int *fails)
{
	for (int i = 0; i < n; i++) {
		rvd_frame_result_t r = rvd_frame_result(ret, empty);
		if (r == RVD_FRAME_STALLED)
			(*stalls)++;
		else if (r == RVD_FRAME_FAILED)
			(*fails)++;
	}
}

TEST a_frame_is_ready(void)
{
	int empty = 3;
	ASSERT_EQ(RVD_FRAME_READY, rvd_frame_result(0, &empty));
	ASSERT_EQ(0, empty);
	PASS();
}

TEST an_empty_run_stalls_once(void)
{
	int empty = 0, stalls = 0, fails = 0;
	feed(-EAGAIN, RVD_FRAME_STALL_POLLS - 1, &empty, &stalls, &fails);
	ASSERT_EQ(0, stalls);
	ASSERT_EQ(RVD_FRAME_STALLED, rvd_frame_result(-EAGAIN, &empty));
	feed(-EAGAIN, 100, &empty, &stalls, &fails);
	ASSERT_EQ(0, stalls);
	ASSERT_EQ(0, fails);
	PASS();
}

TEST a_frame_ends_the_empty_run(void)
{
	int empty = 0, stalls = 0, fails = 0;
	feed(-EAGAIN, RVD_FRAME_STALL_POLLS - 1, &empty, &stalls, &fails);
	feed(0, 1, &empty, &stalls, &fails);
	feed(-EAGAIN, RVD_FRAME_STALL_POLLS - 1, &empty, &stalls, &fails);
	ASSERT_EQ(0, stalls);
	ASSERT_EQ(RVD_FRAME_STALLED, rvd_frame_result(-EAGAIN, &empty));
	PASS();
}

TEST an_error_fails_and_ends_the_empty_run(void)
{
	int empty = 0, stalls = 0, fails = 0;
	feed(-EAGAIN, RVD_FRAME_STALL_POLLS - 1, &empty, &stalls, &fails);
	ASSERT_EQ(RVD_FRAME_FAILED, rvd_frame_result(-EIO, &empty));
	feed(-EAGAIN, RVD_FRAME_STALL_POLLS - 1, &empty, &stalls, &fails);
	ASSERT_EQ(0, stalls);
	PASS();
}

SUITE(frame_result_suite)
{
	RUN_TEST(a_frame_is_ready);
	RUN_TEST(an_empty_run_stalls_once);
	RUN_TEST(a_frame_ends_the_empty_run);
	RUN_TEST(an_error_fails_and_ends_the_empty_run);
}

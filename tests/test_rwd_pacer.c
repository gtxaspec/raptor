#include <stdint.h>

#include "greatest.h"
#include "../rwd/rwd_pacer.h"

TEST rwd_pacer_allows_one_burst_immediately(void)
{
	rwd_pacer_t pacer;
	rwd_pacer_init(&pacer, 16000000, 16000, 1000000);

	ASSERT_EQ(0, rwd_pacer_reserve(&pacer, 8000, 1000000));
	ASSERT_EQ(0, rwd_pacer_reserve(&pacer, 8000, 1000000));
	ASSERT_EQ(8000, rwd_pacer_reserve(&pacer, 1200, 1000000));
	PASS();
}

TEST rwd_pacer_refills_from_elapsed_time(void)
{
	rwd_pacer_t pacer;
	rwd_pacer_init(&pacer, 8000000, 16000, 1000000);

	ASSERT_EQ(0, rwd_pacer_reserve(&pacer, 16000, 1000000));
	ASSERT_EQ(0, rwd_pacer_reserve(&pacer, 8000, 1008000));
	ASSERT_EQ(16000, rwd_pacer_reserve(&pacer, 1200, 1008000));
	PASS();
}

TEST rwd_pacer_preserves_future_reservation(void)
{
	rwd_pacer_t pacer;
	rwd_pacer_init(&pacer, 16000000, 16000, 1000000);

	ASSERT_EQ(0, rwd_pacer_reserve(&pacer, 16000, 1000000));
	ASSERT_EQ(8000, rwd_pacer_reserve(&pacer, 1200, 1000000));
	ASSERT_EQ(0, rwd_pacer_reserve(&pacer, 14800, 1008000));
	ASSERT_EQ(8000, rwd_pacer_reserve(&pacer, 1200, 1008000));
	PASS();
}

TEST rwd_pacer_can_be_disabled(void)
{
	rwd_pacer_t pacer;
	rwd_pacer_init(&pacer, 0, 0, 1000000);

	ASSERT_EQ(0, rwd_pacer_reserve(&pacer, UINT32_MAX, 1000000));
	PASS();
}

SUITE(rwd_pacer_suite)
{
	RUN_TEST(rwd_pacer_allows_one_burst_immediately);
	RUN_TEST(rwd_pacer_refills_from_elapsed_time);
	RUN_TEST(rwd_pacer_preserves_future_reservation);
	RUN_TEST(rwd_pacer_can_be_disabled);
}

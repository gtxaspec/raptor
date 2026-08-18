/*
 * test_idr_recovery.c -- keyframes and explicit requests share one guard
 */

#include "greatest.h"
#include "../rsd/rsd_idr_recovery.h"

TEST idr_recovery_allows_first_request(void)
{
	rsd_idr_recovery_t state;
	rsd_idr_recovery_init(&state);

	ASSERT(rsd_idr_recovery_request_due(&state, 100));
	PASS();
}

TEST idr_recovery_request_starts_guard(void)
{
	rsd_idr_recovery_t state;
	rsd_idr_recovery_init(&state);
	rsd_idr_recovery_note(&state, 5000000);

	ASSERT_FALSE(rsd_idr_recovery_request_due(&state, 5999999));
	ASSERT_FALSE(rsd_idr_recovery_request_due(&state, 6000000));
	ASSERT(rsd_idr_recovery_request_due(&state, 6000001));
	PASS();
}

TEST idr_recovery_natural_keyframe_restarts_guard(void)
{
	rsd_idr_recovery_t state;
	rsd_idr_recovery_init(&state);
	rsd_idr_recovery_note(&state, 5000000); /* requested IDR */
	rsd_idr_recovery_note(&state, 5800000); /* produced keyframe */

	ASSERT_FALSE(rsd_idr_recovery_request_due(&state, 6500000));
	ASSERT_FALSE(rsd_idr_recovery_request_due(&state, 6800000));
	ASSERT(rsd_idr_recovery_request_due(&state, 6800001));
	PASS();
}

TEST idr_recovery_ignores_older_event(void)
{
	rsd_idr_recovery_t state;
	rsd_idr_recovery_init(&state);
	rsd_idr_recovery_note(&state, 5000000);
	rsd_idr_recovery_note(&state, 4000000);

	ASSERT_EQ(5000000, state.last_event_us);
	PASS();
}

SUITE(idr_recovery_suite)
{
	RUN_TEST(idr_recovery_allows_first_request);
	RUN_TEST(idr_recovery_request_starts_guard);
	RUN_TEST(idr_recovery_natural_keyframe_restarts_guard);
	RUN_TEST(idr_recovery_ignores_older_event);
}

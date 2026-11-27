/*
 * test_rsd_slots.c -- idle unauthenticated connections cannot hold the slots
 */

#include "greatest.h"
#include "../rsd/rsd_slots.h"

#define S 1000000LL

TEST an_unauthenticated_connection_expires_after_its_allowance(void)
{
	ASSERT(!rsd_preauth_expired(false, 0, 10 * S, 10 * S));
	ASSERT(rsd_preauth_expired(false, 0, 10 * S + 1, 10 * S));
	PASS();
}

TEST an_authenticated_connection_never_expires_here(void)
{
	ASSERT(!rsd_preauth_expired(true, 0, 3600 * S, 10 * S));
	PASS();
}

TEST the_oldest_unauthenticated_connection_is_evicted(void)
{
	bool authed[4] = {false, true, false, false};
	int64_t at[4] = {50, 10, 20, 30};
	ASSERT_EQ(2, rsd_pick_evictee(authed, at, 4));
	PASS();
}

TEST authenticated_connections_are_never_evicted(void)
{
	bool authed[3] = {true, true, true};
	int64_t at[3] = {1, 2, 3};
	ASSERT_EQ(-1, rsd_pick_evictee(authed, at, 3));
	ASSERT_EQ(-1, rsd_pick_evictee(authed, at, 0));
	PASS();
}

TEST an_old_authenticated_connection_does_not_shield_a_newer_idle_one(void)
{
	bool authed[2] = {true, false};
	int64_t at[2] = {1, 99};
	ASSERT_EQ(1, rsd_pick_evictee(authed, at, 2));
	PASS();
}

SUITE(rsd_slots_suite)
{
	RUN_TEST(an_unauthenticated_connection_expires_after_its_allowance);
	RUN_TEST(an_authenticated_connection_never_expires_here);
	RUN_TEST(the_oldest_unauthenticated_connection_is_evicted);
	RUN_TEST(authenticated_connections_are_never_evicted);
	RUN_TEST(an_old_authenticated_connection_does_not_shield_a_newer_idle_one);
}

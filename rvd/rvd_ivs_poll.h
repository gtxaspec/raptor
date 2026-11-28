/*
 * rvd_ivs_poll.h -- what the IVS thread runs, from what rvd_ivs_start() set up
 */

#ifndef RVD_IVS_POLL_H
#define RVD_IVS_POLL_H

#include <stdbool.h>

typedef enum {
	RVD_IVS_THREAD_NONE, /* nothing to read: rvd_ivs_start() created no algorithm */
	RVD_IVS_THREAD_POLL, /* poll the IVS channel (move, base_move, persondet) */
	RVD_IVS_THREAD_JZDL, /* the standalone JZDL inference thread */
} rvd_ivs_thread_mode_t;

/* jzdl: the standalone JZDL detector was created; algo_handle: the IVS
 * algorithm interface rvd_ivs_start() created, NULL when it failed. With
 * neither there is no channel behind the IVS group, ivs_poll_result fails at
 * once every time, and a poll loop would spin a core until rvd exits. */
static inline rvd_ivs_thread_mode_t rvd_ivs_thread_mode(bool jzdl, const void *algo_handle)
{
	if (jzdl)
		return RVD_IVS_THREAD_JZDL;
	return algo_handle ? RVD_IVS_THREAD_POLL : RVD_IVS_THREAD_NONE;
}

#endif /* RVD_IVS_POLL_H */

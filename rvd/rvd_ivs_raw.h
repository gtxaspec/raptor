/*
 * rvd_ivs_raw.h -- hand one frame from the JZDL thread to a save raw request
 *
 * The JZDL thread owns its channel's frame depth, so a second GetFrame user
 * (save raw) crashes the SDK. The requester parks a buffer here instead and
 * the JZDL thread copies the next frame it receives into it.
 */

#ifndef RVD_IVS_RAW_H
#define RVD_IVS_RAW_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
	pthread_mutex_t lock;
	pthread_cond_t cond; /* CLOCK_MONOTONIC: a wall clock step must not stretch a timeout */
	_Atomic bool want;   /* a request is parked: lets the frame loop skip the lock */
	bool ok;	     /* init succeeded: nothing else may touch an uninitialised lock */
	bool closed;
	int waiters; /* requesters inside grab; close waits for them to leave */
	uint8_t *buf;
	size_t cap;
	uint32_t size, w, h;
	bool done;
} rvd_ivs_raw_t;

int rvd_ivs_raw_init(rvd_ivs_raw_t *r);

/* Refuse new requests, wake a waiting one and wait until it has left, so the
 * state may be torn down. */
void rvd_ivs_raw_close(rvd_ivs_raw_t *r);

static inline bool rvd_ivs_raw_wanted(rvd_ivs_raw_t *r)
{
	return atomic_load(&r->want);
}

/* Copy one frame (at most the requester's capacity) to a parked request. */
void rvd_ivs_raw_serve(rvd_ivs_raw_t *r, const void *data, size_t len, uint32_t w, uint32_t h);

/* Park buf and wait for a frame: 0, or -1 on timeout or close. One request at
 * a time; a second caller waits its turn inside the same timeout. */
int rvd_ivs_raw_grab(rvd_ivs_raw_t *r, uint8_t *buf, size_t cap, uint32_t *size, uint32_t *w,
		     uint32_t *h, int timeout_ms);

#endif /* RVD_IVS_RAW_H */

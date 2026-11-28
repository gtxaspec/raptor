/*
 * rvd_ivs_raw.c -- see rvd_ivs_raw.h
 */

#include <errno.h>
#include <string.h>
#include <time.h>

#include "rvd_ivs_raw.h"

int rvd_ivs_raw_init(rvd_ivs_raw_t *r)
{
	memset(r, 0, sizeof(*r));
	pthread_condattr_t ca;
	if (pthread_condattr_init(&ca) != 0)
		return -1;
	int ret = pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
	if (ret == 0)
		ret = pthread_cond_init(&r->cond, &ca);
	pthread_condattr_destroy(&ca);
	if (ret != 0)
		return -1;
	if (pthread_mutex_init(&r->lock, NULL) != 0) {
		pthread_cond_destroy(&r->cond);
		return -1;
	}
	r->ok = true;
	return 0;
}

void rvd_ivs_raw_close(rvd_ivs_raw_t *r)
{
	if (!r->ok)
		return;
	pthread_mutex_lock(&r->lock);
	r->closed = true;
	pthread_cond_broadcast(&r->cond);
	while (r->waiters > 0)
		pthread_cond_wait(&r->cond, &r->lock);
	pthread_mutex_unlock(&r->lock);
}

void rvd_ivs_raw_serve(rvd_ivs_raw_t *r, const void *data, size_t len, uint32_t w, uint32_t h)
{
	pthread_mutex_lock(&r->lock);
	if (r->buf && !r->done) {
		size_t n = len < r->cap ? len : r->cap;
		memcpy(r->buf, data, n);
		r->size = (uint32_t)n;
		r->w = w;
		r->h = h;
		r->done = true;
		pthread_cond_broadcast(&r->cond);
	}
	pthread_mutex_unlock(&r->lock);
}

int rvd_ivs_raw_grab(rvd_ivs_raw_t *r, uint8_t *buf, size_t cap, uint32_t *size, uint32_t *w,
		     uint32_t *h, int timeout_ms)
{
	struct timespec dl;
	clock_gettime(CLOCK_MONOTONIC, &dl);
	dl.tv_sec += timeout_ms / 1000;
	dl.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
	if (dl.tv_nsec >= 1000000000L) {
		dl.tv_sec++;
		dl.tv_nsec -= 1000000000L;
	}

	if (!r->ok)
		return -1;
	int ret = 0;
	pthread_mutex_lock(&r->lock);
	if (r->closed) {
		pthread_mutex_unlock(&r->lock);
		return -1;
	}
	r->waiters++;
	while (r->buf && !r->closed && ret == 0)
		ret = pthread_cond_timedwait(&r->cond, &r->lock, &dl);
	if (ret == 0 && !r->closed) {
		r->buf = buf;
		r->cap = cap;
		r->done = false;
		atomic_store(&r->want, true);
		while (!r->done && !r->closed && ret == 0)
			ret = pthread_cond_timedwait(&r->cond, &r->lock, &dl);
		if (r->done) {
			*size = r->size;
			*w = r->w;
			*h = r->h;
			ret = 0;
		} else if (ret == 0) {
			ret = ECANCELED;
		}
		r->buf = NULL;
		atomic_store(&r->want, false);
	} else if (ret == 0) {
		ret = ECANCELED;
	}
	r->waiters--;
	pthread_cond_broadcast(&r->cond);
	pthread_mutex_unlock(&r->lock);
	return ret == 0 ? 0 : -1;
}

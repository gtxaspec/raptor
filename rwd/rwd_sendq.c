/*
 * rwd_sendq.c — bounded per-client video send queue (see header).
 */

#include "rwd_sendq.h"

#include <stdlib.h>
#include <string.h>

void rwd_sendq_init(rwd_sendq_t *q)
{
	memset(q, 0, sizeof(*q));
	pthread_mutex_init(&q->lock, NULL);
	pthread_cond_init(&q->cond, NULL);
}

static void purge_locked(rwd_sendq_t *q)
{
	while (q->count > 0) {
		free(q->entries[q->tail].data);
		q->entries[q->tail].data = NULL;
		q->tail = (q->tail + 1) % RWD_SENDQ_SLOTS;
		q->count--;
		q->drops++;
	}
}

int rwd_sendq_push(rwd_sendq_t *q, const uint8_t *data, uint32_t len, uint32_t rtp_ts)
{
	uint8_t *copy = malloc(len);
	if (!copy)
		return -1;
	memcpy(copy, data, len);

	pthread_mutex_lock(&q->lock);
	if (q->shutdown) {
		pthread_mutex_unlock(&q->lock);
		free(copy);
		return -1;
	}
	if (q->count >= RWD_SENDQ_SLOTS) {
		/* The client can't drain at stream rate. Keeping a stale
		 * backlog only adds latency; drop everything and let the
		 * caller resume clean at the next keyframe. */
		purge_locked(q);
		q->drops++; /* the incoming frame */
		pthread_mutex_unlock(&q->lock);
		free(copy);
		return 1;
	}
	rwd_sendq_entry_t *slot = &q->entries[q->head];
	slot->data = copy;
	slot->len = len;
	slot->rtp_ts = rtp_ts;
	q->head = (q->head + 1) % RWD_SENDQ_SLOTS;
	q->count++;
	pthread_cond_signal(&q->cond);
	pthread_mutex_unlock(&q->lock);
	return 0;
}

bool rwd_sendq_pop(rwd_sendq_t *q, rwd_sendq_entry_t *out)
{
	pthread_mutex_lock(&q->lock);
	while (q->count == 0 && !q->shutdown)
		pthread_cond_wait(&q->cond, &q->lock);
	if (q->shutdown) {
		pthread_mutex_unlock(&q->lock);
		return false;
	}
	*out = q->entries[q->tail];
	q->entries[q->tail].data = NULL;
	q->tail = (q->tail + 1) % RWD_SENDQ_SLOTS;
	q->count--;
	pthread_mutex_unlock(&q->lock);
	return true;
}

void rwd_sendq_shutdown(rwd_sendq_t *q)
{
	pthread_mutex_lock(&q->lock);
	q->shutdown = true;
	pthread_cond_broadcast(&q->cond);
	pthread_mutex_unlock(&q->lock);
}

void rwd_sendq_destroy(rwd_sendq_t *q)
{
	pthread_mutex_lock(&q->lock);
	purge_locked(q);
	pthread_mutex_unlock(&q->lock);
	pthread_mutex_destroy(&q->lock);
	pthread_cond_destroy(&q->cond);
}

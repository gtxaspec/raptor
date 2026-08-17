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

void rwd_sendq_fail(rwd_sendq_t *q)
{
	pthread_mutex_lock(&q->lock);
	if (!q->shutdown) {
		purge_locked(q);
		q->needs_keyframe = true;
		q->drops++; /* failed in-flight frame */
	}
	pthread_mutex_unlock(&q->lock);
}

int rwd_sendq_push(rwd_sendq_t *q, const uint8_t *data, uint32_t len, uint32_t rtp_ts,
		   int64_t capture_us, int64_t enqueue_us, bool is_key)
{
	uint8_t *copy = malloc(len);
	if (!copy) {
		rwd_sendq_fail(q);
		return -1;
	}
	memcpy(copy, data, len);

	pthread_mutex_lock(&q->lock);
	if (q->shutdown) {
		pthread_mutex_unlock(&q->lock);
		free(copy);
		return -1;
	}
	if (q->needs_keyframe) {
		if (!is_key) {
			pthread_mutex_unlock(&q->lock);
			free(copy);
			return 1;
		}
		q->needs_keyframe = false;
	}
	if (q->count >= RWD_SENDQ_SLOTS) {
		/* The client can't drain at stream rate. Keeping a stale
		 * backlog only adds latency; drop everything and let the
		 * caller resume clean at the next keyframe. */
		purge_locked(q);
		q->needs_keyframe = true;
		q->drops++; /* the incoming frame */
		pthread_mutex_unlock(&q->lock);
		free(copy);
		return 1;
	}
	rwd_sendq_entry_t *slot = &q->entries[q->head];
	slot->data = copy;
	slot->len = len;
	slot->rtp_ts = rtp_ts;
	slot->capture_us = capture_us;
	slot->enqueue_us = enqueue_us;
	slot->is_key = is_key;
	q->head = (q->head + 1) % RWD_SENDQ_SLOTS;
	q->count++;
	q->enqueued++;
	if (q->count > q->max_depth)
		q->max_depth = q->count;
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
	q->dequeued++;
	pthread_mutex_unlock(&q->lock);
	return true;
}

void rwd_sendq_note_send(rwd_sendq_t *q, const rwd_sendq_entry_t *entry,
			 int64_t send_start_us, int64_t send_end_us, bool success)
{
	if (!q || !entry)
		return;

	int64_t queue_us = send_start_us > entry->enqueue_us ? send_start_us - entry->enqueue_us : 0;
	int64_t send_us = send_end_us > send_start_us ? send_end_us - send_start_us : 0;
	int64_t capture_to_send_us =
		send_end_us > entry->capture_us ? send_end_us - entry->capture_us : 0;

	pthread_mutex_lock(&q->lock);
	if (success) {
		q->sent++;
		q->bytes_sent += entry->len;
	} else {
		q->send_failures++;
	}
	q->last_frame_bytes = entry->len;
	if (entry->len > q->max_frame_bytes)
		q->max_frame_bytes = entry->len;
	q->last_queue_us = queue_us;
	if (queue_us > q->max_queue_us)
		q->max_queue_us = queue_us;
	q->last_send_us = send_us;
	if (send_us > q->max_send_us)
		q->max_send_us = send_us;
	q->last_capture_to_send_us = capture_to_send_us;
	if (capture_to_send_us > q->max_capture_to_send_us)
		q->max_capture_to_send_us = capture_to_send_us;
	pthread_mutex_unlock(&q->lock);
}

void rwd_sendq_get_stats(rwd_sendq_t *q, rwd_sendq_stats_t *stats)
{
	if (!q || !stats)
		return;

	pthread_mutex_lock(&q->lock);
	*stats = (rwd_sendq_stats_t){
		.depth = q->count,
		.max_depth = q->max_depth,
		.enqueued = q->enqueued,
		.dequeued = q->dequeued,
		.sent = q->sent,
		.send_failures = q->send_failures,
		.drops = q->drops,
		.bytes_sent = q->bytes_sent,
		.last_frame_bytes = q->last_frame_bytes,
		.max_frame_bytes = q->max_frame_bytes,
		.last_queue_us = q->last_queue_us,
		.max_queue_us = q->max_queue_us,
		.last_send_us = q->last_send_us,
		.max_send_us = q->max_send_us,
		.last_capture_to_send_us = q->last_capture_to_send_us,
		.max_capture_to_send_us = q->max_capture_to_send_us,
	};
	pthread_mutex_unlock(&q->lock);
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

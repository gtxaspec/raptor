/*
 * rsd555_queue.c -- Inter-thread frame queue with refcounted shared buffers
 *
 * Reader thread allocates one shared frame per ring read, then pushes
 * a lightweight reference to each client queue. The data is copied once
 * from the ring; clients share the same allocation via atomic refcount.
 */

#include "rsd555.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>

/* ── Shared frame (refcounted) ── */

rsd555_shared_frame_t *rsd555_shared_frame_new(const uint8_t *data, uint32_t len,
					       int64_t timestamp_us, uint16_t nal_type,
					       uint8_t is_key)
{
	rsd555_shared_frame_t *sf = malloc(sizeof(*sf) + len);
	if (!sf)
		return NULL;
	sf->refcnt = 1;
	sf->len = len;
	sf->timestamp_us = timestamp_us;
	sf->nal_type = nal_type;
	sf->is_key = is_key;
	memcpy(sf->data, data, len);
	return sf;
}

void rsd555_shared_frame_ref(rsd555_shared_frame_t *sf)
{
	__atomic_add_fetch(&sf->refcnt, 1, __ATOMIC_RELAXED);
}

void rsd555_shared_frame_unref(rsd555_shared_frame_t *sf)
{
	if (__atomic_sub_fetch(&sf->refcnt, 1, __ATOMIC_ACQ_REL) == 0)
		free(sf);
}

/* ── Queue ── */

int rsd555_queue_init(rsd555_frame_queue_t *q, int max_frames)
{
	memset(q, 0, sizeof(*q));
	q->max_count = max_frames;
	if (pthread_mutex_init(&q->lock, NULL) != 0)
		return -1;
	q->event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (q->event_fd < 0) {
		pthread_mutex_destroy(&q->lock);
		return -1;
	}
	return 0;
}

void rsd555_queue_destroy(rsd555_frame_queue_t *q)
{
	while (q->head) {
		rsd555_frame_t *f = q->head;
		q->head = f->next;
		rsd555_shared_frame_unref(f->shared);
		free(f);
	}
	if (q->event_fd >= 0)
		close(q->event_fd);
	pthread_mutex_destroy(&q->lock);
}

int rsd555_queue_push_ref(rsd555_frame_queue_t *q, rsd555_shared_frame_t *sf)
{
	rsd555_frame_t *f = malloc(sizeof(*f));
	if (!f)
		return -1;
	rsd555_shared_frame_ref(sf);
	f->shared = sf;
	f->next = NULL;

	pthread_mutex_lock(&q->lock);

	while (q->count >= q->max_count && q->head) {
		rsd555_frame_t *old = q->head;
		q->head = old->next;
		if (!q->head)
			q->tail = NULL;
		q->count--;
		rsd555_shared_frame_unref(old->shared);
		free(old);
	}

	if (q->tail)
		q->tail->next = f;
	else
		q->head = f;
	q->tail = f;
	q->count++;

	pthread_mutex_unlock(&q->lock);

	uint64_t val = 1;
	(void)write(q->event_fd, &val, sizeof(val));

	return 0;
}

rsd555_frame_t *rsd555_queue_pop(rsd555_frame_queue_t *q)
{
	pthread_mutex_lock(&q->lock);
	rsd555_frame_t *f = q->head;
	if (f) {
		q->head = f->next;
		if (!q->head)
			q->tail = NULL;
		q->count--;
		f->next = NULL;
	}
	pthread_mutex_unlock(&q->lock);
	return f;
}

void rsd555_frame_free(rsd555_frame_t *f)
{
	if (!f)
		return;
	rsd555_shared_frame_unref(f->shared);
	free(f);
}

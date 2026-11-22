/*
 * rmr_prebuf.c -- Process-local circular pre-buffer
 *
 * Frames are stored in a contiguous data region with a simple
 * bump-pointer allocator that wraps to offset 0 when a frame
 * doesn't fit at the tail. Slot metadata tracks offset/length
 * into the data region.
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "rmr_prebuf.h"

rmr_prebuf_t *rmr_prebuf_create(uint32_t capacity, uint32_t data_size)
{
	if (capacity == 0 || (capacity & (capacity - 1)) != 0)
		return NULL; /* must be power of 2 */
	if (data_size == 0)
		return NULL;

	rmr_prebuf_t *pb = calloc(1, sizeof(*pb));
	if (!pb)
		return NULL;

	pb->slots = calloc(capacity, sizeof(rmr_prebuf_slot_t));
	pb->data = malloc(data_size);
	if (!pb->slots || !pb->data) {
		free(pb->slots);
		free(pb->data);
		free(pb);
		return NULL;
	}

	pb->capacity = capacity;
	pb->mask = capacity - 1;
	pb->data_size = data_size;
	return pb;
}

void rmr_prebuf_destroy(rmr_prebuf_t *pb)
{
	if (!pb)
		return;
	free(pb->slots);
	free(pb->data);
	free(pb);
}

void rmr_prebuf_set_growth(rmr_prebuf_t *pb, uint32_t max_size, int64_t keep_us)
{
	if (!pb)
		return;
	pb->data_max = max_size;
	pb->keep_us = keep_us;
}

/* Frames a write of len at offset would retire, oldest first */
static uint32_t overlapped(const rmr_prebuf_t *pb, uint32_t offset, uint32_t len)
{
	uint32_t oldest = pb->write_idx - pb->count;
	uint32_t evict = 0;
	for (uint32_t i = 0; i < pb->count; i++) {
		const rmr_prebuf_slot_t *s = &pb->slots[(oldest + i) & pb->mask];
		if (s->data_offset < offset + len && offset < s->data_offset + s->data_length)
			evict = i + 1;
	}
	return evict;
}

/* Enlarge the region to size (capped), packing the live frames from offset 0 */
static int resize(rmr_prebuf_t *pb, uint64_t want)
{
	uint32_t size = want > pb->data_max ? pb->data_max : (uint32_t)want;
	if (size <= pb->data_size)
		return -1;
	uint8_t *nd = malloc(size);
	if (!nd)
		return -1;
	uint32_t oldest = pb->write_idx - pb->count, head = 0;
	for (uint32_t i = 0; i < pb->count; i++) {
		rmr_prebuf_slot_t *s = &pb->slots[(oldest + i) & pb->mask];
		memcpy(nd + head, pb->data + s->data_offset, s->data_length);
		s->data_offset = head;
		head += s->data_length;
	}
	free(pb->data);
	pb->data = nd;
	pb->data_size = size;
	pb->data_head = head;
	return 0;
}

static int grow(rmr_prebuf_t *pb)
{
	return resize(pb, (uint64_t)pb->data_size * 2);
}

int rmr_prebuf_reserve(rmr_prebuf_t *pb, uint32_t size)
{
	if (!pb)
		return -1;
	if (size <= pb->data_size || pb->data_max <= pb->data_size)
		return 0;
	return resize(pb, size);
}

static uint32_t place(const rmr_prebuf_t *pb, uint32_t len)
{
	return pb->data_head + len > pb->data_size ? 0 : pb->data_head;
}

void rmr_prebuf_push(rmr_prebuf_t *pb, const uint8_t *data, uint32_t len, int64_t timestamp,
		     uint8_t is_key)
{
	if (!pb || !data || len == 0)
		return;
	while (len > pb->data_size && pb->data_max > pb->data_size && grow(pb) == 0)
		;
	if (len > pb->data_size)
		return;

	/* Retire every frame up to the newest one this write overlaps. The
	 * slot cap alone is not enough: at a bitrate above the sizing
	 * estimate the data region wraps first, and the oldest slots would
	 * replay overwritten bytes. After a wrap the overlapped frames are
	 * newer than the ones left at the tail, hence "up to the newest". */
	uint32_t offset = place(pb, len);
	uint32_t evict = overlapped(pb, offset, len);
	while (evict > 0 && pb->data_max > pb->data_size) {
		uint32_t last = (pb->write_idx - pb->count + evict - 1) & pb->mask;
		if (timestamp - pb->slots[last].timestamp >= pb->keep_us || grow(pb) < 0)
			break;
		offset = place(pb, len);
		evict = overlapped(pb, offset, len);
	}
	pb->count -= evict;

	memcpy(pb->data + offset, data, len);
	pb->data_head = offset + len;

	/* Write slot metadata */
	uint32_t idx = pb->write_idx & pb->mask;
	pb->slots[idx].data_offset = offset;
	pb->slots[idx].data_length = len;
	pb->slots[idx].timestamp = timestamp;
	pb->slots[idx].is_key = is_key;

	pb->write_idx++;
	if (pb->count < pb->capacity)
		pb->count++;
}

uint32_t rmr_prebuf_find_keyframe(const rmr_prebuf_t *pb, int64_t max_age_us)
{
	if (!pb || pb->count == 0)
		return UINT32_MAX;

	/* Walk backwards from newest. Find the first keyframe that
	 * gives us at least max_age_us of pre-buffer duration. */
	uint32_t newest_idx = (pb->write_idx - 1) & pb->mask;
	int64_t newest_ts = pb->slots[newest_idx].timestamp;

	uint32_t start = pb->write_idx - pb->count;
	uint32_t best = UINT32_MAX;

	for (uint32_t i = pb->write_idx; i > start; i--) {
		uint32_t si = (i - 1) & pb->mask;
		if (!pb->slots[si].is_key)
			continue;
		int64_t age = newest_ts - pb->slots[si].timestamp;
		if (age >= max_age_us)
			return i - 1; /* meets or exceeds target duration */
		best = i - 1;	      /* track — use if nothing older qualifies */
	}
	return best; /* oldest keyframe in buffer (may be < target) */
}

uint32_t rmr_prebuf_find_frame_at(const rmr_prebuf_t *pb, int64_t min_ts)
{
	if (!pb || pb->count == 0)
		return UINT32_MAX;

	uint32_t start = pb->write_idx - pb->count;
	for (uint32_t i = start; i < pb->write_idx; i++) {
		uint32_t si = i & pb->mask;
		if (pb->slots[si].timestamp >= min_ts)
			return i;
	}
	return UINT32_MAX;
}

int rmr_prebuf_iterate(const rmr_prebuf_t *pb, uint32_t start_idx, rmr_prebuf_iter_fn fn, void *ctx)
{
	if (!pb || !fn || pb->count == 0)
		return -1;

	/* Validate start_idx is within valid range */
	uint32_t oldest = pb->write_idx - pb->count;
	if (start_idx < oldest || start_idx >= pb->write_idx)
		return -1;

	int iterated = 0;
	for (uint32_t i = start_idx; i < pb->write_idx; i++) {
		uint32_t si = i & pb->mask;
		const rmr_prebuf_slot_t *slot = &pb->slots[si];
		if (fn(slot, pb->data + slot->data_offset, ctx) != 0)
			break;
		iterated++;
	}
	return iterated;
}

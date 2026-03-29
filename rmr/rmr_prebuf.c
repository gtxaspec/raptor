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

void rmr_prebuf_push(rmr_prebuf_t *pb, const uint8_t *data, uint32_t len, int64_t timestamp,
		     uint8_t is_key)
{
	if (!pb || !data || len == 0 || len > pb->data_size)
		return;

	/* Allocate space in data region. If the frame doesn't fit at
	 * the current head, wrap to offset 0. */
	uint32_t offset = pb->data_head;
	if (offset + len > pb->data_size)
		offset = 0;

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

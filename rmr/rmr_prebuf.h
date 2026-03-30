/*
 * rmr_prebuf.h -- Process-local circular pre-buffer for motion clip recording
 *
 * Stores the last N seconds of AVCC video or raw audio frames in a
 * local ring so they can be replayed into a clip when motion triggers.
 * Independent of the SHM ring (which has a 64-slot limit too small
 * for 5 seconds of history).
 */

#ifndef RMR_PREBUF_H
#define RMR_PREBUF_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
	uint32_t data_offset; /* byte offset into data region */
	uint32_t data_length; /* frame payload size */
	int64_t timestamp;    /* capture timestamp (us, from ring) */
	uint8_t is_key;	      /* 1 if keyframe (video only) */
} rmr_prebuf_slot_t;

typedef struct {
	rmr_prebuf_slot_t *slots;
	uint32_t capacity;  /* max slot count (power of 2) */
	uint32_t mask;	    /* capacity - 1 */
	uint32_t write_idx; /* next write position (monotonic, wraps via mask) */
	uint32_t count;	    /* number of valid slots (0..capacity) */

	uint8_t *data;	    /* contiguous data region */
	uint32_t data_size; /* total data region size */
	uint32_t data_head; /* next write offset in data region */
} rmr_prebuf_t;

/* Create pre-buffer. capacity must be power of 2. */
rmr_prebuf_t *rmr_prebuf_create(uint32_t capacity, uint32_t data_size);

/* Destroy and free all resources. */
void rmr_prebuf_destroy(rmr_prebuf_t *pb);

/* Push a frame into the pre-buffer. Evicts oldest if full. */
void rmr_prebuf_push(rmr_prebuf_t *pb, const uint8_t *data, uint32_t len, int64_t timestamp,
		     uint8_t is_key);

/* Find the oldest keyframe within max_age_us of the newest frame.
 * Returns the absolute slot index, or UINT32_MAX if none found. */
uint32_t rmr_prebuf_find_keyframe(const rmr_prebuf_t *pb, int64_t max_age_us);

/* Find the oldest frame with timestamp >= min_ts.
 * Returns the absolute slot index, or UINT32_MAX if none found. */
uint32_t rmr_prebuf_find_frame_at(const rmr_prebuf_t *pb, int64_t min_ts);

/* Callback for iteration. Return 0 to continue, non-zero to stop. */
typedef int (*rmr_prebuf_iter_fn)(const rmr_prebuf_slot_t *slot, const uint8_t *data, void *ctx);

/* Iterate frames from start_idx to the current head.
 * Returns number of frames iterated, or -1 on error. */
int rmr_prebuf_iterate(const rmr_prebuf_t *pb, uint32_t start_idx, rmr_prebuf_iter_fn fn,
		       void *ctx);

/* Return the number of valid frames in the buffer. */
static inline uint32_t rmr_prebuf_count(const rmr_prebuf_t *pb)
{
	return pb ? pb->count : 0;
}

/* Return the timestamp of a slot by absolute index. */
static inline int64_t rmr_prebuf_timestamp(const rmr_prebuf_t *pb, uint32_t abs_idx)
{
	return pb->slots[abs_idx & pb->mask].timestamp;
}

/* Return the absolute index of the newest slot. */
static inline uint32_t rmr_prebuf_newest(const rmr_prebuf_t *pb)
{
	return (pb->write_idx - 1) & pb->mask;
}

/* Return the current write index (monotonic, for arithmetic). */
static inline uint32_t rmr_prebuf_write_idx(const rmr_prebuf_t *pb)
{
	return pb->write_idx;
}

#endif /* RMR_PREBUF_H */

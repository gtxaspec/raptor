/*
 * rmr_streams.h -- [recording] streams: which rings rmr records
 */

#ifndef RMR_STREAMS_H
#define RMR_STREAMS_H

#include <stdint.h>

#define RMR_MAX_TRACKS	  3
#define RMR_RING_NAME_MAX 32
#define RMR_TAG_MAX	  16

typedef struct {
	char ring[RMR_RING_NAME_MAX];
	char tag[RMR_TAG_MAX]; /* file suffix; empty for the first track */
} rmr_stream_spec_t;

/* Ring name of the legacy [recording] stream index (0 = main). */
const char *rmr_stream_ring_name(int idx);

/* Parse "main, s1_main:R" into at most max specs. NULL or empty maps the
 * legacy index alone. The first track is never tagged; a later one
 * without a tag takes its ring name up to the first '_' (s1_main -> s1),
 * or the whole name when that is taken. Bad, duplicate or surplus
 * entries are skipped with a warning. Returns the count, always >= 1. */
int rmr_streams_parse(const char *spec, int legacy_idx, rmr_stream_spec_t *out, int max);

/* Pre-roll data region for a stream observed at bytes_per_sec: the
 * replay window (prebuffer_sec + 1) plus a quarter of headroom, at
 * least 1 MB, at most max_bytes. */
uint32_t rmr_prebuf_size_for_rate(uint64_t bytes_per_sec, int prebuffer_sec, uint32_t max_bytes);

#endif /* RMR_STREAMS_H */

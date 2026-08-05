/*
 * rmr_storage.h -- Recording file rotation and storage management
 */

#ifndef RMR_STORAGE_H
#define RMR_STORAGE_H

#include <stdint.h>
#include <stdbool.h>

typedef struct rmr_storage rmr_storage_t;

typedef struct {
	const char *base_path; /* e.g. /mnt/mmcblk0p1/raptor */
	int segment_minutes;   /* segment duration (default 5) */
	int segment_seconds;   /* nonzero overrides minutes (debug/test) */
	int max_storage_mb;    /* 0 = unlimited */
} rmr_storage_config_t;

/* Create/destroy storage manager */
rmr_storage_t *rmr_storage_create(const rmr_storage_config_t *cfg);
void rmr_storage_destroy(rmr_storage_t *st);

/* Open a new segment file. Creates date directory if needed.
 * Returns fd on success, -1 on error. Writes path to path_out. */
int rmr_storage_open_segment(rmr_storage_t *st, char *path_out, int path_out_size);

/* Close a segment file. */
void rmr_storage_close_segment(int fd);

/* Next wall-clock boundary at a multiple of the segment length,
 * strictly after now_rt_us. Pure -- times injected for testability. */
int64_t rmr_storage_next_boundary(int64_t now_rt_us, int segment_len_sec);

/* Segment length in seconds (segment_seconds override or minutes). */
int rmr_storage_segment_len_sec(rmr_storage_t *st);

/* Wall-clock-aligned rotation: rotate once now crosses the first
 * boundary after the segment's open time (both CLOCK_REALTIME), with
 * a 5s floor so a boundary-adjacent open or a backward NTP step
 * cannot spray micro-clips. */
bool rmr_storage_should_rotate_at(rmr_storage_t *st, int64_t start_rt_us, int64_t now_rt_us);

/* Delete oldest recordings until total size is under max_storage_mb.
 * Returns number of files deleted, or -1 on error. */
int rmr_storage_enforce_limit(rmr_storage_t *st);

/* Check if storage path is mounted and writable. */
bool rmr_storage_available(rmr_storage_t *st);

#endif /* RMR_STORAGE_H */

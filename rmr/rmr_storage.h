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
	int scan_interval;     /* quota rescan cadence, seconds; -1 = never */
	int rotate_fsync;      /* fsync on segment close (default true);
				* false relies on flushes + close() flush */
} rmr_storage_config_t;

/* Create/destroy storage manager */
rmr_storage_t *rmr_storage_create(const rmr_storage_config_t *cfg);
void rmr_storage_destroy(rmr_storage_t *st);

/* Open a new segment file. Creates date directory if needed.
 * Returns fd on success, -1 on error. Writes path to path_out. */
int rmr_storage_open_segment(rmr_storage_t *st, char *path_out, int path_out_size);

/* Close a segment file. fsyncs first when the storage's rotate_fsync
 * policy is on (default), so the segment content is durable before the
 * fd disappears. */
void rmr_storage_close_segment(rmr_storage_t *st, int fd);

/* Open the NEXT segment's file and pre-build its filename now, so the
 * rotation critical path is finalize + fsync + fd swap instead of
 * directory scan + create + first-byte latency. Returns fd on success,
 * -1 on error. Writes path to path_out. Call near the END of a segment
 * (at a keyframe); adopt the fd via rmr_storage_activate_segment. */
int rmr_storage_preopen_segment(rmr_storage_t *st, char *path_out, int path_out_size);

/* Adopt a preopened fd as the active segment. The caller keeps using
 * the fd; the storage layer just makes its create durable (fsync now
 * that activation is on the rotation path, not the write path) and
 * clears its pending slot. */
void rmr_storage_activate_segment(rmr_storage_t *st, int fd);

/* True when a preopened fd is waiting to be adopted. */
bool rmr_storage_preopen_ready(const rmr_storage_t *st);

/* Close and remove a pending preopened fd and its (never written)
 * file. No-op when nothing is pending. */
void rmr_storage_discard_preopen(rmr_storage_t *st);

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
 * Internal: runs on the storage monitor thread only. */
int rmr_storage_enforce_limit(rmr_storage_t *st);

/* Record loop calls this after closing a segment; the monitor thread
 * wakes and rescans the quota at most once per scan interval
 * (recording.quota_scan_sec: default 60, 0 = every rotation,
 * -1 = never scan). Never blocks the caller. */
void rmr_storage_note_rotation(rmr_storage_t *st);

/* Start/stop the storage monitor thread that owns quota scanning. */
void rmr_storage_start_monitor(rmr_storage_t *st);
void rmr_storage_stop_monitor(rmr_storage_t *st);

/* Check if storage path is mounted and writable. */
bool rmr_storage_available(rmr_storage_t *st);

#endif /* RMR_STORAGE_H */

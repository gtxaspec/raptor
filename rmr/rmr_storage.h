/*
 * rmr_storage.h -- Recording file rotation and storage management
 */

#ifndef RMR_STORAGE_H
#define RMR_STORAGE_H

#include <stdint.h>
#include <stdbool.h>

typedef struct rmr_storage rmr_storage_t;

typedef struct {
	const char *base_path; /* e.g. /mnt/mmcblk0p1/raptor; NULL or "" = the
				* largest card partition (rmr_storage_pick_root) */
	const char *subdir;    /* under a picked root, e.g. "/clips" */
	int segment_minutes;   /* segment duration (default 5) */
	int segment_seconds;   /* nonzero overrides minutes (debug/test) */
	int max_storage_mb;    /* 0 = unlimited */
	int min_free_mb;       /* keep this much free on the medium, 0 = off */
	bool hour_dirs;	       /* YYYY-MM-DD/HH/ instead of YYYY-MM-DD/ */
	int cleanup_delay_ms;  /* background cleanup waits this long after a
				* request and paces its deletes; 0 = at once */
	int repick_ms;	       /* picked root: how often to look for a larger
				* card partition; 0 = 10 s */
} rmr_storage_config_t;

#define RMR_STORAGE_DEFAULT_ROOT "/mnt/mmcblk0p1/raptor"

/* A picked root is never a card partition smaller than this: the small
 * FAT boot/update partition beside a recording one is not for video. */
#define RMR_AUTO_ROOT_MIN_MB 512

/* "<mount>/raptor" on the largest mounted card partition (/dev/mmcblk*
 * under /mnt) of at least RMR_AUTO_ROOT_MIN_MB. Returns 0, or -1 with
 * RMR_STORAGE_DEFAULT_ROOT when there is none. */
int rmr_storage_pick_root(char *out, int out_size);

/* Tests: where rmr_storage_pick_root reads mounts, which mount points
 * count, how it sizes them and what it falls back to. NULL restores each
 * default (/proc/mounts, "/mnt/", statvfs, RMR_STORAGE_DEFAULT_ROOT). */
struct statvfs;
typedef int (*rmr_statvfs_fn)(const char *path, struct statvfs *buf);
void rmr_storage_set_pick_source(const char *mounts, const char *mnt_prefix, rmr_statvfs_fn fs,
				 const char *fallback_root);

/* True when a quota or a free-space floor makes room for new files */
bool rmr_storage_loops(const rmr_storage_t *st);

/* The directory files go to now */
const char *rmr_storage_path(const rmr_storage_t *st);

/* Create/destroy storage manager */
rmr_storage_t *rmr_storage_create(const rmr_storage_config_t *cfg);
void rmr_storage_destroy(rmr_storage_t *st);

/* Open a new segment file. Creates date directory if needed.
 * Returns fd on success, -1 on error. Writes path to path_out. */
int rmr_storage_open_segment(rmr_storage_t *st, char *path_out, int path_out_size);

/* Open a file named for the wall-clock time rt_us, "_<suffix>" before
 * ".mp4" when suffix is non-empty. Returns fd or -1. */
int rmr_storage_open_file(rmr_storage_t *st, int64_t rt_us, const char *suffix, char *path_out,
			  int path_out_size);

/* One file name shared by every track that opens a file for the same
 * period, so a front HH-MM-SS.mp4 pairs with its HH-MM-SS_R.mp4. */
typedef struct {
	int64_t rt_us; /* wall clock the shared name is formed from */
	int64_t until; /* shared while now < until, in the caller's clock */
	uint32_t gen;  /* 0 = none yet */
} rmr_name_stamp_t;

/* Name time for a track opening a file now. The current stamp is reused
 * while it is live and this track (*used) has not opened a file under it
 * yet; otherwise rt_us starts a new stamp valid until `until`. A track
 * reopening within one period never reuses its own name, so it cannot
 * truncate the file it just closed. */
int64_t rmr_name_stamp_take(rmr_name_stamp_t *s, uint32_t *used, int64_t now, int64_t rt_us,
			    int64_t until);

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

/* Run the cleanup on a background thread (started on first use) */
void rmr_storage_request_cleanup(rmr_storage_t *st);

/* Check if storage path is mounted and writable. */
bool rmr_storage_available(rmr_storage_t *st);

#endif /* RMR_STORAGE_H */

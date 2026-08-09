/*
 * rmr_timelapse.h -- timelapse sampling state machine
 *
 * Pure logic, no I/O and no clock of its own: the caller injects
 * monotonic time into tick(), ring sequence numbers into take(), and
 * the sample's wall-clock date into needs_rotate(). That keeps every
 * decision this module makes -- cadence, stall recovery, sample
 * dedup, rotation, timestamp spacing -- unit-testable on the host
 * without a daemon.
 *
 * Flow per frame-loop iteration:
 *   rmr_tl_tick(t, now)                  arm when a sample is due
 *   if (is_key && rmr_tl_take(t, seq))   claim this keyframe
 *       if (rmr_tl_needs_rotate(t, day)) close file; open new one;
 *                                        rmr_tl_file_opened(t, day)
 *       dts = rmr_tl_sample_dts90(t)     write sample at this DTS
 *
 * A stall longer than one interval arms exactly one sample and
 * reschedules from now: a timelapse never back-fills a burst of
 * frames for time that produced none.
 */

#ifndef RMR_TIMELAPSE_H
#define RMR_TIMELAPSE_H

#include <stdint.h>
#include <stdbool.h>

/* Clamp bounds. Below 2s the sampler degenerates to taking every
 * GOP keyframe and the IDR requests start to matter to rate control;
 * file_frames under a minute of playback makes rotation churn. */
#define RMR_TL_MIN_INTERVAL_SEC 2
#define RMR_TL_MIN_FPS		1
#define RMR_TL_MAX_FPS		120
#define RMR_TL_MIN_FILE_FRAMES	60

typedef struct {
	int64_t interval_us;
	uint32_t playback_fps;
	uint32_t file_frames; /* 0 = rotate daily, else rotate every N frames */

	bool want_sample;
	int64_t next_due_us; /* 0 = arm on first tick */
	uint64_t last_seq;   /* ring seq of the last claimed sample */

	int32_t file_day; /* yyyymmdd of the open file, 0 = none */
	uint32_t frames_in_file;
	uint64_t frame_idx; /* since file open; drives DTS */
} rmr_tl_t;

/* Initialise with clamped config. file_frames 0 keeps daily rotation. */
void rmr_tl_init(rmr_tl_t *t, int interval_sec, int playback_fps, int file_frames);

/* Live re-configuration. Interval takes effect at the next schedule.
 * A playback rate change returns true and the caller must rotate the
 * file: the DTS timeline restarts per file and cannot bend mid-track. */
void rmr_tl_set_interval(rmr_tl_t *t, int interval_sec);
bool rmr_tl_set_playback_fps(rmr_tl_t *t, int playback_fps);

/* Arm want_sample when due. now_us is monotonic. */
void rmr_tl_tick(rmr_tl_t *t, int64_t now_us);

/* Force the next keyframe to be sampled (raptorctl timelapse-snap). */
void rmr_tl_force(rmr_tl_t *t);

/* Claim a keyframe for the armed sample. False when not armed or when
 * this ring seq was already claimed (one IDR never satisfies two ticks). */
bool rmr_tl_take(rmr_tl_t *t, uint64_t seq);

/* The ring was recreated and its sequence space restarted: forget the
 * claimed-seq guard, or every frame of the new ring reads as already
 * sampled and the timelapse silently stops. */
void rmr_tl_ring_reset(rmr_tl_t *t);

/* True when the claimed sample must open a new file first. sample_day
 * is yyyymmdd of the sample's wall-clock time; in file_frames mode the
 * date is ignored and a file may legitimately span midnight. */
bool rmr_tl_needs_rotate(const rmr_tl_t *t, int32_t sample_day);

/* Record that a file for sample_day is open; resets the DTS timeline. */
void rmr_tl_file_opened(rmr_tl_t *t, int32_t sample_day);

/* 90 kHz DTS for the sample about to be written; advances the index.
 * Computed from the frame index, not accumulated, so rates that do not
 * divide 90000 still land exact on every whole second. */
int64_t rmr_tl_sample_dts90(rmr_tl_t *t);

#endif /* RMR_TIMELAPSE_H */

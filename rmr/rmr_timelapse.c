/*
 * rmr_timelapse.c -- timelapse sampling state machine
 *
 * See rmr_timelapse.h for the contract. Everything here is pure state
 * on injected time, sequence numbers, and dates; the daemon owns all
 * I/O, file naming, and clock reads.
 */

#include "rmr_timelapse.h"

void rmr_tl_init(rmr_tl_t *t, int interval_sec, int playback_fps, int file_frames)
{
	if (interval_sec < RMR_TL_MIN_INTERVAL_SEC)
		interval_sec = RMR_TL_MIN_INTERVAL_SEC;
	if (playback_fps < RMR_TL_MIN_FPS)
		playback_fps = RMR_TL_MIN_FPS;
	if (playback_fps > RMR_TL_MAX_FPS)
		playback_fps = RMR_TL_MAX_FPS;
	if (file_frames != 0 && file_frames < RMR_TL_MIN_FILE_FRAMES)
		file_frames = RMR_TL_MIN_FILE_FRAMES;

	t->interval_us = (int64_t)interval_sec * 1000000;
	t->playback_fps = (uint32_t)playback_fps;
	t->file_frames = (uint32_t)file_frames;
	t->want_sample = false;
	t->next_due_us = 0;
	t->last_seq = 0;
	t->file_day = 0;
	t->frames_in_file = 0;
	t->frame_idx = 0;
}

void rmr_tl_set_interval(rmr_tl_t *t, int interval_sec)
{
	if (interval_sec < RMR_TL_MIN_INTERVAL_SEC)
		interval_sec = RMR_TL_MIN_INTERVAL_SEC;
	t->interval_us = (int64_t)interval_sec * 1000000;
}

bool rmr_tl_set_playback_fps(rmr_tl_t *t, int playback_fps)
{
	if (playback_fps < RMR_TL_MIN_FPS)
		playback_fps = RMR_TL_MIN_FPS;
	if (playback_fps > RMR_TL_MAX_FPS)
		playback_fps = RMR_TL_MAX_FPS;
	if ((uint32_t)playback_fps == t->playback_fps)
		return false;
	t->playback_fps = (uint32_t)playback_fps;
	return true;
}

void rmr_tl_tick(rmr_tl_t *t, int64_t now_us)
{
	if (t->next_due_us == 0) {
		t->want_sample = true;
		t->next_due_us = now_us + t->interval_us;
		return;
	}
	if (now_us < t->next_due_us)
		return;

	t->want_sample = true;
	t->next_due_us += t->interval_us;
	/* A stall swallowed whole intervals: one sample, rescheduled
	 * from now. A timelapse never back-fills time that produced
	 * no frames. */
	if (t->next_due_us <= now_us)
		t->next_due_us = now_us + t->interval_us;
}

void rmr_tl_force(rmr_tl_t *t)
{
	t->want_sample = true;
}

bool rmr_tl_take(rmr_tl_t *t, uint64_t seq)
{
	if (!t->want_sample || seq <= t->last_seq)
		return false;
	t->last_seq = seq;
	t->want_sample = false;
	return true;
}

void rmr_tl_ring_reset(rmr_tl_t *t)
{
	t->last_seq = 0;
}

bool rmr_tl_needs_rotate(const rmr_tl_t *t, int32_t sample_day)
{
	if (t->file_day == 0)
		return true;
	if (t->file_frames != 0)
		return t->frames_in_file >= t->file_frames;
	return sample_day != t->file_day;
}

void rmr_tl_file_opened(rmr_tl_t *t, int32_t sample_day)
{
	t->file_day = sample_day;
	t->frames_in_file = 0;
	t->frame_idx = 0;
}

int64_t rmr_tl_sample_dts90(rmr_tl_t *t)
{
	int64_t dts = (int64_t)(t->frame_idx * 90000ULL / t->playback_fps);

	t->frame_idx++;
	t->frames_in_file++;
	return dts;
}

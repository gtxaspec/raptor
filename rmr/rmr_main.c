/*
 * rmr_main.c -- Raptor Media Recorder
 *
 * Reads H.264/H.265 video + audio from SHM rings and writes
 * fragmented MP4 files to SD card. Single-threaded: the main loop
 * reads rings, feeds the muxer, and writes directly to disk.
 *
 * Motion clips with pre-buffer: a process-local circular buffer
 * stores the last N seconds of frames. When motion triggers, the
 * pre-buffer is replayed into a clip file before live frames continue.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/select.h>

#include "rmr.h"

/* ── Write callbacks ── */

static int direct_write(const void *buf, uint32_t len, void *ctx)
{
	rmr_state_t *st = ctx;
	if (len == 0)
		return 0;

	int fd = st->segment_fd;
	if (fd < 0)
		return -1;

	const uint8_t *p = buf;
	uint32_t remaining = len;
	while (remaining > 0) {
		ssize_t n = write(fd, p, remaining);
		if (n > 0) {
			p += n;
			remaining -= (uint32_t)n;
			st->bytes_written += (uint64_t)n;
		} else if (n < 0) {
			if (errno == EINTR)
				continue;
			RSS_ERROR("write error: %s", strerror(errno));
			if (errno == ENOSPC) {
				RSS_ERROR("SD card full, stopping recording");
				atomic_store(&st->recording, false);
			} else {
				/* Storage failed mid-segment (I/O error,
				 * NFS soft-mount timeout): the file has
				 * holes where the lost writes should be.
				 * The current segment is poisoned; abort
				 * it and let the normal flow start a
				 * fresh one at the next keyframe. */
				st->segment_write_error = true;
			}
			return -1;
		}
	}
	if (st->sign_enabled)
		rmr_sign_stream_update(&st->sign_seg, buf, len);
	return 0;
}

static int clip_write(const void *buf, uint32_t len, void *ctx)
{
	rmr_state_t *st = ctx;
	if (len == 0)
		return 0;

	int fd = st->clip_fd;
	if (fd < 0)
		return -1;

	const uint8_t *p = buf;
	uint32_t remaining = len;
	while (remaining > 0) {
		ssize_t n = write(fd, p, remaining);
		if (n > 0) {
			p += n;
			remaining -= (uint32_t)n;
			st->clip_bytes += (uint64_t)n;
		} else if (n < 0) {
			if (errno == EINTR)
				continue;
			RSS_ERROR("clip write error: %s", strerror(errno));
			st->clip_write_error = true;
			return -1;
		}
	}
	if (st->sign_enabled)
		rmr_sign_stream_update(&st->sign_clip, buf, len);
	return 0;
}

static int tl_write(const void *buf, uint32_t len, void *ctx)
{
	rmr_state_t *st = ctx;
	if (len == 0)
		return 0;

	int fd = st->tl_fd;
	if (fd < 0)
		return -1;

	const uint8_t *p = buf;
	uint32_t remaining = len;
	while (remaining > 0) {
		ssize_t n = write(fd, p, remaining);
		if (n > 0) {
			p += n;
			remaining -= (uint32_t)n;
			st->tl_bytes += (uint64_t)n;
		} else if (n < 0) {
			if (errno == EINTR)
				continue;
			RSS_ERROR("timelapse write error: %s", strerror(errno));
			st->tl_write_error = true;
			return -1;
		}
	}
	if (st->sign_enabled)
		rmr_sign_stream_update(&st->sign_tl, buf, len);
	return 0;
}

/* ── Segment management ── */

static void setup_mux_video_track_dur(rmr_mux_t *mux, rmr_state_t *st, uint32_t default_duration)
{
	rmr_video_params_t vp = {
		.codec = (st->video_codec == 2)	  ? RMR_CODEC_MJPEG
			 : (st->video_codec == 1) ? RMR_CODEC_H265
						  : RMR_CODEC_H264,
		.width = st->width,
		.height = st->height,
		.timescale = 90000,
		.default_duration = default_duration,
	};
	if (st->video_codec == 2)
		rmr_mux_set_video(mux, &vp, NULL, 0, NULL, 0, NULL, 0);
	else
		rmr_mux_set_video(mux, &vp, st->params.sps, st->params.sps_len, st->params.pps,
				  st->params.pps_len,
				  st->params.vps_len > 0 ? st->params.vps : NULL,
				  st->params.vps_len);
}

static void setup_mux_video_track(rmr_mux_t *mux, rmr_state_t *st)
{
	setup_mux_video_track_dur(mux, st, 0);
}

static void setup_mux_tracks(rmr_mux_t *mux, rmr_state_t *st)
{
	setup_mux_video_track(mux, st);

	if (st->audio_ring) {
		rmr_audio_params_t ap = {
			.sample_rate = st->audio_sample_rate, .channels = 1, .aot = st->audio_aot};
		switch (st->audio_codec) {
		case RMR_AUDIO_PCMU:
			ap.codec = RMR_AUDIO_PCMU;
			ap.bits_per_sample = 8;
			break;
		case RMR_AUDIO_PCMA:
			ap.codec = RMR_AUDIO_PCMA;
			ap.bits_per_sample = 8;
			break;
		case RMR_AUDIO_AAC:
			ap.codec = RMR_AUDIO_AAC;
			ap.bits_per_sample = 16;
			break;
		case RMR_AUDIO_OPUS:
			ap.codec = RMR_AUDIO_OPUS;
			ap.bits_per_sample = 16;
			break;
		default:
			ap.codec = RMR_AUDIO_L16;
			ap.bits_per_sample = 16;
			break;
		}
		rmr_mux_set_audio(mux, &ap);
	}
}

static int start_segment(rmr_state_t *st)
{
	int fd = rmr_storage_open_segment(st->storage, st->segment_path, sizeof(st->segment_path));
	if (fd < 0)
		return -1;

	st->mux = rmr_mux_create(direct_write, st);
	if (!st->mux) {
		rmr_storage_close_segment(fd);
		return -1;
	}

	setup_mux_tracks(st->mux, st);
	st->segment_fd = fd;
	if (st->sign_enabled)
		rmr_sign_stream_begin(&st->sign_seg, &st->sign_key);
	rmr_mux_start(st->mux);
	if (st->sign_enabled)
		rmr_sign_stream_emit(&st->sign_seg, false, direct_write, st);
	st->segment_start_us = rss_timestamp_us();
	st->segment_start_rt_us = rss_wallclock_us();
	st->segment_boundary_rt_us = rmr_storage_next_boundary(
		st->segment_start_rt_us, rmr_storage_segment_len_sec(st->storage));
	st->segment_idr_requested = false;

	RSS_INFO("recording segment: %s", st->segment_path);
	return 0;
}

static void close_segment(rmr_state_t *st)
{
	if (st->mux) {
		if (!st->segment_write_error && rmr_mux_finalize(st->mux) < 0)
			st->segment_write_error = true;
		if (!st->segment_write_error && st->sign_enabled)
			rmr_sign_stream_emit(&st->sign_seg, true, direct_write, st);
		rmr_mux_destroy(st->mux);
		st->mux = NULL;
	}

	int fd = st->segment_fd;
	st->segment_fd = -1;

	if (fd >= 0) {
		rmr_storage_close_segment(fd);
		if (st->segment_write_error)
			RSS_WARN("segment aborted after write error, file kept: %s",
				 st->segment_path);
		else
			RSS_DEBUG("segment closed: %s (%" PRIu64 " frames, %" PRIu64
				  " bytes)",
				  st->segment_path, st->frames_written, st->bytes_written);
	}
	st->segment_write_error = false;
}

/* ── Motion clip management ── */

static int open_clip(rmr_state_t *st)
{
	if (!st->clip_storage)
		return -1;

	int fd = rmr_storage_open_segment(st->clip_storage, st->clip_path, sizeof(st->clip_path));
	if (fd < 0)
		return -1;

	st->clip_mux = rmr_mux_create(clip_write, st);
	if (!st->clip_mux) {
		rmr_storage_close_segment(fd);
		return -1;
	}

	st->clip_fd = fd;
	st->clip_v_ts_base = -1;
	st->clip_a_dts = 0;
	st->clip_a_ts_base = -1;
	st->clip_start_us = rss_timestamp_us();
	st->clip_bytes = 0;
	st->clip_write_error = false;

	setup_mux_tracks(st->clip_mux, st);
	if (st->sign_enabled)
		rmr_sign_stream_begin(&st->sign_clip, &st->sign_key);
	rmr_mux_start(st->clip_mux);
	if (st->sign_enabled)
		rmr_sign_stream_emit(&st->sign_clip, false, clip_write, st);

	RSS_INFO("motion clip started: %s", st->clip_path);
	return 0;
}

static void close_clip(rmr_state_t *st)
{
	if (st->clip_mux) {
		if (!st->clip_write_error && rmr_mux_finalize(st->clip_mux) < 0)
			st->clip_write_error = true;
		if (!st->clip_write_error && st->sign_enabled)
			rmr_sign_stream_emit(&st->sign_clip, true, clip_write, st);
		rmr_mux_destroy(st->clip_mux);
		st->clip_mux = NULL;
	}
	if (st->clip_fd >= 0) {
		rmr_storage_close_segment(st->clip_fd);
		if (st->clip_write_error)
			RSS_WARN("motion clip aborted after write error, file kept: %s",
				 st->clip_path);
		else
			RSS_DEBUG("motion clip closed: %s (%" PRIu64 " bytes)", st->clip_path,
				  st->clip_bytes);
		st->clip_fd = -1;
	}
	st->clip_write_error = false;
}

/* ── Timelapse file management ── */

/* Local calendar date as yyyymmdd; drives daily rotation, so files
 * split at the user's midnight, matching the storage date dirs. */
static int32_t rmr_local_day(void)
{
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	return (int32_t)((tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday);
}

static void close_timelapse(rmr_state_t *st)
{
	if (st->tl_mux) {
		rmr_mux_finalize(st->tl_mux);
		if (st->sign_enabled)
			rmr_sign_stream_emit(&st->sign_tl, true, tl_write, st);
		rmr_mux_destroy(st->tl_mux);
		st->tl_mux = NULL;
	}
	if (st->tl_fd >= 0) {
		rmr_storage_close_segment(st->tl_fd);
		if (st->tl_write_error)
			RSS_WARN("timelapse file aborted after write error, file kept: %s",
				 st->tl_path);
		else
			RSS_INFO("timelapse file closed: %s (%u frames, %" PRIu64 " bytes)",
				 st->tl_path, st->tl.frames_in_file, st->tl_bytes);
		st->tl_fd = -1;
	}
	st->tl.file_day = 0;
	st->tl_write_error = false;
}

static int open_timelapse(rmr_state_t *st, int32_t day)
{
	if (!st->tl_storage)
		return -1;

	int fd = rmr_storage_open_segment(st->tl_storage, st->tl_path, sizeof(st->tl_path));
	if (fd < 0) {
		/* One warn per minute: storage failures repeat every tick
		 * and a timelapse must never own the syslog ring. */
		int64_t now = rss_timestamp_us();
		if (now - st->tl_last_err_us >= 60000000) {
			RSS_WARN("timelapse: cannot open file under %s", st->tl_path);
			st->tl_last_err_us = now;
		}
		return -1;
	}

	st->tl_mux = rmr_mux_create(tl_write, st);
	if (!st->tl_mux) {
		rmr_storage_close_segment(fd);
		return -1;
	}

	st->tl_fd = fd;
	st->tl_bytes = 0;
	st->tl_write_error = false;

	/* Fragment-per-sample: without a next sample to derive from, each
	 * sample's declared duration must be the playback grid step or the
	 * file contradicts its own DTS grid and players drop frames. */
	setup_mux_video_track_dur(st->tl_mux, st, 90000 / st->tl.playback_fps);
	if (st->sign_enabled)
		rmr_sign_stream_begin(&st->sign_tl, &st->sign_key);
	rmr_mux_start(st->tl_mux);
	if (st->sign_enabled)
		rmr_sign_stream_emit(&st->sign_tl, false, tl_write, st);

	rmr_tl_file_opened(&st->tl, day);
	RSS_INFO("timelapse file: %s (1 frame per %llds, %u fps playback)", st->tl_path,
		 (long long)(st->tl.interval_us / 1000000), st->tl.playback_fps);
	return 0;
}

/* Write one claimed sample; rotates the file when the sampler says so. */
static void timelapse_write_sample(rmr_state_t *st, const uint8_t *data, uint32_t len)
{
	int32_t day = rmr_local_day();

	if (rmr_tl_needs_rotate(&st->tl, day)) {
		close_timelapse(st);
		if (open_timelapse(st, day) < 0)
			return;
	}
	if (!st->tl_mux)
		return;

	/* Every sample is a keyframe: fragment-per-sample keeps the file
	 * valid to the last written frame and chains a signature box per
	 * sample, mirroring the per-GOP pattern of the other writers. */
	if (rmr_mux_flush_fragment(st->tl_mux) < 0) {
		/* Abort the poisoned file; the next sample reopens. */
		st->tl_write_error = true;
		close_timelapse(st);
		return;
	}
	if (st->sign_enabled)
		rmr_sign_stream_emit(&st->sign_tl, false, tl_write, st);

	int64_t dts = rmr_tl_sample_dts90(&st->tl);
	rmr_video_sample_t vs = {
		.data = data,
		.size = len,
		.dts = dts,
		.pts = dts,
		.is_key = true,
	};
	if (rmr_mux_write_video(st->tl_mux, &vs) == 0)
		st->tl_frames_total++;
}

/* Write a video frame to the clip mux with independent DTS. */
static void clip_write_video(rmr_state_t *st, const uint8_t *avcc, uint32_t avcc_len,
			     int64_t timestamp, bool is_key)
{
	if (!st->clip_mux)
		return;

	if (st->clip_v_ts_base < 0)
		st->clip_v_ts_base = timestamp;

	int64_t v_dts = (timestamp - st->clip_v_ts_base) * 90 / 1000;
	rmr_video_sample_t vs = {
		.data = avcc,
		.size = avcc_len,
		.dts = v_dts,
		.pts = v_dts,
		.is_key = is_key,
	};

	if (is_key) {
		if (rmr_mux_flush_fragment(st->clip_mux) < 0)
			st->clip_write_error = true;
		if (st->sign_enabled && !st->clip_write_error)
			rmr_sign_stream_emit(&st->sign_clip, false, clip_write, st);
	}
	rmr_mux_write_video(st->clip_mux, &vs);
}

/*
 * Steer a smooth audio DTS counter (sample units) toward the ring
 * capture time so recordings inherit rad's wall-slewed clock instead
 * of free-running on frame count, which drifts with the source clock
 * and turns ring-overflow gaps into a permanent A/V offset. rsp
 * pattern: snap forward on real gaps (>4 frames, the file gets an
 * honest gap), re-base the mapping if ring time regresses (rad
 * restart — DTS must stay monotonic), nudge 1ms inside the band.
 * *ts_base anchors the ring-time-to-DTS mapping; < 0 = unset.
 */
static int64_t steer_audio_dts(int64_t dts, int64_t ring_ts, int64_t *ts_base, uint32_t samples,
			       uint32_t sample_rate)
{
	if (sample_rate == 0)
		return dts;
	if (*ts_base < 0)
		*ts_base = ring_ts - dts * 1000000 / sample_rate;
	int64_t target = (ring_ts - *ts_base) * sample_rate / 1000000;
	int64_t err = target - dts;
	int64_t nudge = sample_rate / 1000; /* 1ms */

	if (err > (int64_t)samples * 4)
		return target;
	if (err < -(int64_t)samples * 4) {
		*ts_base = ring_ts - dts * 1000000 / sample_rate;
		return dts;
	}
	if (err > nudge)
		return dts + nudge;
	if (err < -nudge)
		return dts - nudge;
	return dts;
}

/* Write an audio frame to the clip mux with independent DTS. */
static void clip_write_audio(rmr_state_t *st, const uint8_t *data, uint32_t len, uint32_t samples,
			     int64_t ring_ts)
{
	if (!st->clip_mux)
		return;

	st->clip_a_dts = steer_audio_dts(st->clip_a_dts, ring_ts, &st->clip_a_ts_base, samples,
					 st->audio_sample_rate);
	rmr_audio_sample_t as = {
		.data = data,
		.size = len,
		.dts = st->clip_a_dts,
	};
	rmr_mux_write_audio(st->clip_mux, &as);
	st->clip_a_dts += samples;
}

/* ── Pre-buffer replay ── */

typedef struct {
	rmr_state_t *st;
	uint32_t audio_samples_per_frame;
	uint32_t audio_bps;
	int max_frames; /* limit replay count, -1 = unlimited */
	int count;	/* frames replayed so far */
} replay_ctx_t;

static int replay_video_frame(const rmr_prebuf_slot_t *slot, const uint8_t *data, void *ctx)
{
	replay_ctx_t *rc = ctx;
	clip_write_video(rc->st, data, slot->data_length, slot->timestamp, slot->is_key);
	rc->count++;
	return 0;
}

static int replay_audio_frame(const rmr_prebuf_slot_t *slot, const uint8_t *data, void *ctx)
{
	replay_ctx_t *rc = ctx;
	if (rc->max_frames >= 0 && rc->count >= rc->max_frames)
		return 1; /* stop iteration */
	uint32_t samples = rc->audio_samples_per_frame ? rc->audio_samples_per_frame
						       : slot->data_length / rc->audio_bps;
	clip_write_audio(rc->st, data, slot->data_length, samples, slot->timestamp);
	rc->count++;
	return 0;
}

static int open_clip_with_prebuffer(rmr_state_t *st, uint32_t audio_samples_per_frame,
				    uint32_t audio_bps)
{
	if (open_clip(st) < 0)
		return -1;

	/* Search one extra second back so the keyframe-aligned pre-buffer
	 * always meets or exceeds the configured duration. */
	int64_t max_age_us = ((int64_t)st->prebuffer_sec + 1) * 1000000;

	/* Find the oldest keyframe within the pre-buffer window */
	uint32_t vstart = rmr_prebuf_find_keyframe(st->video_pb, max_age_us);
	if (vstart == UINT32_MAX) {
		RSS_WARN("no keyframe in pre-buffer, clip starts without pre-buffer");
		return 0;
	}

	/* Get the video pre-buffer time range */
	int64_t kf_ts = rmr_prebuf_timestamp(st->video_pb, vstart);
	int64_t newest_v_ts = rmr_prebuf_timestamp(st->video_pb, rmr_prebuf_newest(st->video_pb));
	int64_t v_duration_us = newest_v_ts - kf_ts;

	replay_ctx_t rc = {
		.st = st,
		.audio_samples_per_frame = audio_samples_per_frame,
		.audio_bps = audio_bps,
		.max_frames = -1,
		.count = 0,
	};

	/* Replay video pre-buffer */
	int vcount = rmr_prebuf_iterate(st->video_pb, vstart, replay_video_frame, &rc);
	RSS_DEBUG("pre-buffer: replayed %d video frames (%.1fs)", vcount,
		  v_duration_us / 1000000.0);

	/* Replay audio pre-buffer — match video duration by frame count.
	 * Timestamp matching is unreliable across rings, so calculate
	 * how many audio frames fit in the video pre-buffer duration. */
	if (st->audio_pb && rmr_prebuf_count(st->audio_pb) > 0 && v_duration_us > 0) {
		/* Audio frame duration in microseconds */
		int64_t audio_frame_us;
		if (audio_samples_per_frame > 0 && st->audio_sample_rate > 0)
			audio_frame_us =
				(int64_t)audio_samples_per_frame * 1000000 / st->audio_sample_rate;
		else
			audio_frame_us = 20000; /* fallback: 20ms */

		int audio_frame_count = (int)(v_duration_us / audio_frame_us);
		if (audio_frame_count < 1)
			audio_frame_count = 1;
		if ((uint32_t)audio_frame_count > rmr_prebuf_count(st->audio_pb))
			audio_frame_count = (int)rmr_prebuf_count(st->audio_pb);

		/* Rewind from the head by audio_frame_count frames */
		uint32_t astart = rmr_prebuf_write_idx(st->audio_pb) - (uint32_t)audio_frame_count;

		rc.max_frames = audio_frame_count;
		rc.count = 0;
		int acount = rmr_prebuf_iterate(st->audio_pb, astart, replay_audio_frame, &rc);
		RSS_DEBUG("pre-buffer: replayed %d audio frames (target %d, %.1fs)", acount,
			  audio_frame_count, v_duration_us / 1000000.0);
	}

	return 0;
}

/* ── Control socket handler ── */

static int rmr_ctrl_handler(const char *cmd_json, char *resp_buf, int resp_buf_size, void *userdata)
{
	rmr_state_t *st = userdata;

	int common =
		rss_ctrl_handle_common(cmd_json, resp_buf, resp_buf_size, st->cfg, st->config_path);
	if (common >= 0)
		return common;

	char cmd[64];
	if (rss_json_get_str(cmd_json, "cmd", cmd, sizeof(cmd)) != 0)
		return rss_ctrl_resp_error(resp_buf, resp_buf_size, "missing cmd");

	if (strcmp(cmd, "enable") == 0) {
		atomic_store(&st->recording, true);
		if (st->mode == RMR_MODE_BOTH)
			atomic_store(&st->clip_recording, true);
		return rss_ctrl_resp_ok(resp_buf, resp_buf_size);
	}

	if (strcmp(cmd, "disable") == 0) {
		atomic_store(&st->recording, false);
		atomic_store(&st->clip_recording, false);
		return rss_ctrl_resp_ok(resp_buf, resp_buf_size);
	}

	if (strcmp(cmd, "start") == 0) {
		if (st->mode == RMR_MODE_MOTION)
			atomic_store(&st->recording, true);
		if (st->mode == RMR_MODE_BOTH)
			atomic_store(&st->clip_recording, true);
		return rss_ctrl_resp_ok(resp_buf, resp_buf_size);
	}

	if (strcmp(cmd, "stop") == 0) {
		if (st->mode == RMR_MODE_MOTION)
			atomic_store(&st->recording, false);
		if (st->mode == RMR_MODE_BOTH)
			atomic_store(&st->clip_recording, false);
		return rss_ctrl_resp_ok(resp_buf, resp_buf_size);
	}

	if (strcmp(cmd, "status") == 0) {
		static const char *mode_names[] = {"continuous", "motion", "both"};
		cJSON *r = cJSON_CreateObject();
		cJSON_AddBoolToObject(r, "recording", atomic_load(&st->recording));
		cJSON_AddBoolToObject(r, "clip", atomic_load(&st->clip_recording));
		cJSON_AddStringToObject(r, "mode", mode_names[st->mode]);
		cJSON_AddStringToObject(r, "file", st->segment_path);
		cJSON_AddNumberToObject(r, "frames", (double)st->frames_written);
		cJSON_AddNumberToObject(r, "dropped", (double)st->frames_dropped);
		cJSON_AddNumberToObject(r, "bytes", (double)st->bytes_written);
		cJSON_AddBoolToObject(r, "sign", st->sign_enabled);
		cJSON_AddBoolToObject(r, "sei_timecode", st->sei_timecode);
		cJSON_AddBoolToObject(r, "timelapse", st->tl_enabled);
		return rss_ctrl_resp_json(resp_buf, resp_buf_size, r);
	}

	if (strcmp(cmd, "sign-status") == 0) {
		cJSON *r = cJSON_CreateObject();
		cJSON_AddBoolToObject(r, "enabled", st->sign_enabled);
		if (st->sign_enabled) {
			char fp[17];
			for (int i = 0; i < 8; i++)
				snprintf(fp + i * 2, 3, "%02x", st->sign_key.fingerprint[i]);
			cJSON_AddStringToObject(r, "fingerprint", fp);
		}
		return rss_ctrl_resp_json(resp_buf, resp_buf_size, r);
	}

	if (strcmp(cmd, "export-pubkey") == 0) {
		if (!st->sign_enabled)
			return rss_ctrl_resp_error(resp_buf, resp_buf_size, "signing disabled");
		char hex[65];
		for (int i = 0; i < 32; i++)
			snprintf(hex + i * 2, 3, "%02x", st->sign_key.public[i]);
		cJSON *r = cJSON_CreateObject();
		cJSON_AddStringToObject(r, "pubkey", hex);
		return rss_ctrl_resp_json(resp_buf, resp_buf_size, r);
	}

	if (strcmp(cmd, "timelapse-enable") == 0) {
		st->tl_enabled = true;
		rss_config_set_bool(st->cfg, "timelapse", "enabled", true);
		return rss_ctrl_resp_ok(resp_buf, resp_buf_size);
	}

	if (strcmp(cmd, "timelapse-disable") == 0) {
		st->tl_enabled = false;
		rss_config_set_bool(st->cfg, "timelapse", "enabled", false);
		if (st->tl_mux)
			close_timelapse(st);
		return rss_ctrl_resp_ok(resp_buf, resp_buf_size);
	}

	if (strcmp(cmd, "timelapse-snap") == 0) {
		if (!st->tl_enabled)
			return rss_ctrl_resp_error(resp_buf, resp_buf_size, "timelapse disabled");
		rmr_tl_force(&st->tl);
		if (st->video_ring && st->video_codec != 2)
			rss_ring_request_idr(st->video_ring);
		return rss_ctrl_resp_ok(resp_buf, resp_buf_size);
	}

	if (strcmp(cmd, "timelapse-set") == 0) {
		char key[32];
		int val;
		if (rss_json_get_str(cmd_json, "key", key, sizeof(key)) != 0 ||
		    rss_json_get_int(cmd_json, "value", &val) != 0)
			return rss_ctrl_resp_error(resp_buf, resp_buf_size, "need key and value");
		if (strcmp(key, "interval") == 0) {
			rmr_tl_set_interval(&st->tl, val);
			rss_config_set_int(st->cfg, "timelapse", "interval",
					   (int)(st->tl.interval_us / 1000000));
		} else if (strcmp(key, "playback_fps") == 0) {
			/* The DTS timeline cannot bend mid-track: a rate
			 * change rotates the file. */
			if (rmr_tl_set_playback_fps(&st->tl, val) && st->tl_mux)
				close_timelapse(st);
			rss_config_set_int(st->cfg, "timelapse", "playback_fps",
					   (int)st->tl.playback_fps);
		} else {
			return rss_ctrl_resp_error(resp_buf, resp_buf_size, "unknown key");
		}
		return rss_ctrl_resp_ok(resp_buf, resp_buf_size);
	}

	if (strcmp(cmd, "timelapse-status") == 0) {
		cJSON *r = cJSON_CreateObject();
		if (!r)
			return rss_ctrl_resp_error(resp_buf, resp_buf_size, "alloc");
		cJSON_AddBoolToObject(r, "enabled", st->tl_enabled);
		cJSON_AddNumberToObject(r, "interval", (double)(st->tl.interval_us / 1000000));
		cJSON_AddNumberToObject(r, "playback_fps", st->tl.playback_fps);
		cJSON_AddNumberToObject(r, "file_frames", st->tl.file_frames);
		cJSON_AddStringToObject(r, "file", st->tl_mux ? st->tl_path : "");
		cJSON_AddNumberToObject(r, "frames_in_file", st->tl.frames_in_file);
		cJSON_AddNumberToObject(r, "frames_total", (double)st->tl_frames_total);
		cJSON_AddNumberToObject(r, "bytes", (double)st->tl_bytes);
		return rss_ctrl_resp_json(resp_buf, resp_buf_size, r);
	}

	return rss_ctrl_resp_error(resp_buf, resp_buf_size, "unknown command");
}

/* ── Main loop ── */

static void record_loop(rmr_state_t *st)
{
	int64_t v_ts_base = -1;	 /* continuous segment video timestamp base */
	bool v_wait_key = false; /* drop video until a keyframe after ring overflow */

	/* Audio DTS increment per ring frame. */
	uint32_t audio_bps =
		(st->audio_codec == RMR_AUDIO_PCMU || st->audio_codec == RMR_AUDIO_PCMA) ? 1 : 2;
	uint32_t audio_samples_per_frame = 0;
	if (st->audio_codec == RMR_AUDIO_AAC) {
		/* HE-AAC frames carry 2048 samples; the producer declares
		 * the size in the ring header. */
		audio_samples_per_frame = 1024;
		if (st->audio_ring) {
			const rss_ring_header_t *ahdr = rss_ring_get_header(st->audio_ring);
			if (ahdr->width)
				audio_samples_per_frame = ahdr->width;
		}
	} else if (st->audio_codec == RMR_AUDIO_OPUS) {
		audio_samples_per_frame = st->audio_sample_rate / 50;
	}
	int64_t a_dts_counter = 0;
	int64_t a_ts_base = -1; /* ring ts mapping base for a_dts_counter steering */
	bool was_recording = false;

	int ctrl_fd = st->ctrl ? rss_ctrl_get_fd(st->ctrl) : -1;
	int audio_retry = 0;
	uint64_t last_video_ws = 0;
	int video_idle = 0;
	uint64_t last_audio_ws = 0;
	int audio_idle = 0;
	bool standby = false; /* no writable destination: detached from rings */

	while (rss_running(st->running)) {
		/* Handle control socket */
		if (ctrl_fd >= 0) {
			fd_set fds;
			struct timeval tv = {0, 0};
			FD_ZERO(&fds);
			FD_SET(ctrl_fd, &fds);
			if (select(ctrl_fd + 1, &fds, NULL, NULL, &tv) > 0)
				rss_ctrl_accept_and_handle(st->ctrl, rmr_ctrl_handler, st);
		}

		/* ── Storage standby ── */
		/* With no writable destination there is no work: detach from
		 * the rings instead of consuming frames only to drop them. A
		 * reader with nowhere to write falls behind, every overflow
		 * requests an IDR, and the extra keyframes cost every OTHER
		 * consumer quality at a fixed bitrate. Poll for media at 1Hz;
		 * the reconnect paths below reattach when it appears, and
		 * pre-roll staleness is age-bounded by the prebuffer window. */
		bool have_dest = rmr_storage_available(st->storage) ||
				 rmr_storage_available(st->clip_storage);
		if (!have_dest) {
			if (!standby) {
				standby = true;
				if (st->mux) {
					close_segment(st);
					was_recording = false;
				}
				if (st->clip_mux)
					close_clip(st);
				if (st->video_ring) {
					rss_ring_release(st->video_ring);
					rss_ring_close(st->video_ring);
					st->video_ring = NULL;
				}
				if (st->audio_ring) {
					rss_ring_close(st->audio_ring);
					st->audio_ring = NULL;
				}
				RSS_WARN("no writable storage: standing by, rings released");
			}
			usleep(1000000);
			continue;
		}
		if (standby) {
			standby = false;
			RSS_INFO("storage available: resuming");
		}

		/* Lazy audio ring attach — retry every ~2s until found */
		if (st->audio_enabled && !st->audio_ring && ++audio_retry >= 50) {
			audio_retry = 0;
			st->audio_ring = rss_ring_open("audio");
			if (st->audio_ring) {
				rss_ring_check_version(st->audio_ring, "audio");
				const rss_ring_header_t *ahdr = rss_ring_get_header(st->audio_ring);
				st->audio_codec = ahdr->codec;
				st->audio_sample_rate = ahdr->fps_num;
				/* Start reading from current position (skip stale frames) */
				st->audio_read_seq = atomic_load(&ahdr->write_seq);
				/* Update audio codec params */
				audio_bps = (st->audio_codec == RMR_AUDIO_PCMU ||
					     st->audio_codec == RMR_AUDIO_PCMA)
						    ? 1
						    : 2;
				st->audio_aot = ahdr->profile;
				audio_samples_per_frame = 0;
				if (st->audio_codec == RMR_AUDIO_AAC)
					audio_samples_per_frame = ahdr->width ? ahdr->width : 1024;
				else if (st->audio_codec == RMR_AUDIO_OPUS)
					audio_samples_per_frame = st->audio_sample_rate / 50;
				RSS_DEBUG("audio ring attached (late): codec=%u rate=%u "
					  "frame_samples=%u aot=%u",
					  st->audio_codec, st->audio_sample_rate,
					  audio_samples_per_frame, st->audio_aot);
			}
		}

		/* ── Reconnect video ring if RVD restarted ── */
		if (!st->video_ring) {
			st->video_ring = rss_ring_open(st->video_ring_name);
			if (st->video_ring) {
				rss_ring_check_version(st->video_ring, "video");
				const rss_ring_header_t *vhdr = rss_ring_get_header(st->video_ring);
				uint32_t mfs = rss_ring_max_frame_size(st->video_ring);
				if (mfs > st->frame_buf_size) {
					uint8_t *new_frame = malloc(mfs);
					uint8_t *new_avcc = malloc(mfs + RSS_SEI_TS_MAX);
					if (!new_frame || !new_avcc) {
						free(new_frame);
						free(new_avcc);
						rss_ring_close(st->video_ring);
						st->video_ring = NULL;
						continue;
					}
					free(st->frame_buf);
					free(st->avcc_buf);
					st->frame_buf = new_frame;
					st->avcc_buf = new_avcc;
					st->frame_buf_size = mfs;
					st->avcc_buf_size = mfs + RSS_SEI_TS_MAX;
				}
				st->video_codec = vhdr->codec;
				st->video_read_seq = 0;
				st->params.ready = false;
				video_idle = 0;
				last_video_ws = 0;
				rss_ring_acquire(st->video_ring);
				/* Codec or geometry may have changed: a
				 * timelapse track cannot bend mid-file. The new
				 * ring also restarts its sequence space, so the
				 * sampler's claimed-seq guard must forget the
				 * old one or it rejects every new frame. */
				if (st->tl_mux)
					close_timelapse(st);
				rmr_tl_ring_reset(&st->tl);
				RSS_DEBUG("video ring reconnected (%s)", st->video_ring_name);
			} else {
				usleep(200000);
			}
			continue;
		}

		/* ── Read video frame from ring ── */
		uint32_t length;
		rss_ring_slot_t meta;
		int ret = rss_ring_read(st->video_ring, &st->video_read_seq, st->frame_buf,
					st->frame_buf_size, &length, &meta);

		if (ret == RSS_EOVERFLOW) {
			/* Frames were lost: anything until the next keyframe
			 * references pictures we never read, and writing it
			 * puts undecodable GOP fragments in the recording. */
			rss_ring_request_idr(st->video_ring);
			st->frames_dropped++;
			v_wait_key = true;
			continue;
		}
		if (ret == -EAGAIN) {
			const rss_ring_header_t *vhdr = rss_ring_get_header(st->video_ring);
			uint64_t ws = vhdr->write_seq;
			if (ws == last_video_ws)
				video_idle++;
			else
				video_idle = 0;
			last_video_ws = ws;
			if (video_idle >= 50) { /* ~2s at 40ms wait */
				RSS_DEBUG("video ring idle, closing (%s)", st->video_ring_name);
				rss_ring_release(st->video_ring);
				rss_ring_close(st->video_ring);
				st->video_ring = NULL;
				video_idle = 0;
			} else {
				rss_ring_wait(st->video_ring, 40);
			}
			continue;
		}
		if (ret != 0)
			continue;
		video_idle = 0;

		if (v_wait_key) {
			if (!meta.is_key) {
				st->frames_dropped++;
				continue;
			}
			v_wait_key = false;
		}

		/* Prepare frame data for muxer */
		const uint8_t *mux_data;
		uint32_t mux_len;
		bool is_mjpeg = (st->video_codec == 2);

		if (is_mjpeg) {
			if (!st->params.ready)
				st->params.ready = true;
			mux_data = st->frame_buf;
			mux_len = length;
		} else {
			if (meta.is_key && !st->params.ready)
				rmr_extract_params(st->frame_buf, length, st->video_codec,
						   &st->params);
			if (!st->params.ready)
				continue;

			/* ST 0604 timecode SEI leads the sample; capture UTC
			 * comes from the producer's ring clock mapping. */
			uint32_t sei_len = 0;
			int64_t utc_off;
			uint8_t utc_st;
			if (st->sei_timecode &&
			    rss_ring_get_utc(st->video_ring, &utc_off, &utc_st) == 0) {
				int n = rss_sei_build_timestamp(
					st->avcc_buf, st->avcc_buf_size, (int)st->video_codec,
					RSS_SEI_PREFIX_AVCC, (uint64_t)(meta.timestamp + utc_off),
					utc_st);
				if (n > 0)
					sei_len = (uint32_t)n;
			}

			int avcc_len =
				rmr_annexb_to_avcc(st->frame_buf, length, st->avcc_buf + sei_len,
						   st->avcc_buf_size - sei_len, st->video_codec);
			if (avcc_len <= 0)
				continue;

			if (avcc_len >= 5) {
				const uint8_t *fb = st->avcc_buf + sei_len;
				uint32_t nal_len = ((uint32_t)fb[0] << 24) |
						   ((uint32_t)fb[1] << 16) |
						   ((uint32_t)fb[2] << 8) | (uint32_t)fb[3];
				if (nal_len > (uint32_t)avcc_len - 4) {
					RSS_WARN("AVCC corrupt: nal_len=%u avcc_len=%d", nal_len,
						 avcc_len);
					continue;
				}
			}
			mux_data = st->avcc_buf;
			mux_len = sei_len + (uint32_t)avcc_len;
		}

		/* Push to video pre-buffer (always, for motion clips) */
		if (st->video_pb)
			rmr_prebuf_push(st->video_pb, mux_data, mux_len, meta.timestamp,
					meta.is_key);

		/* ── Read audio frames and push to pre-buffer ── */
		/* Audio frame data saved for writing below */
		struct {
			uint8_t data[8192];
			uint32_t len;
			uint32_t samples;
			int64_t ts; /* ring capture time (us) */
		} audio_frames[4];
		int audio_count = 0;

		if (st->audio_ring) {
			for (int burst = 0; burst < 4; burst++) {
				uint32_t alen;
				rss_ring_slot_t ameta;
				ret = rss_ring_read(st->audio_ring, &st->audio_read_seq,
						    st->audio_buf, sizeof(st->audio_buf), &alen,
						    &ameta);
				if (ret == RSS_EOVERFLOW || ret != 0)
					break;
				audio_idle = 0;

				/* Push to audio pre-buffer */
				if (st->audio_pb)
					rmr_prebuf_push(st->audio_pb, st->audio_buf, alen,
							ameta.timestamp, 0);

				/* Save for writing to muxers */
				if (audio_count < 4) {
					memcpy(audio_frames[audio_count].data, st->audio_buf, alen);
					audio_frames[audio_count].len = alen;
					audio_frames[audio_count].samples =
						audio_samples_per_frame ? audio_samples_per_frame
									: alen / audio_bps;
					audio_frames[audio_count].ts = ameta.timestamp;
					audio_count++;
				}
			}

			/* Idle detection — close audio ring if RAD stopped */
			const rss_ring_header_t *ah = rss_ring_get_header(st->audio_ring);
			uint64_t aws = ah->write_seq;
			if (aws == last_audio_ws)
				audio_idle++;
			else
				audio_idle = 0;
			last_audio_ws = aws;
			if (audio_idle >= 100) { /* ~2s at 50Hz poll */
				RSS_DEBUG("audio ring idle, closing");
				rss_ring_close(st->audio_ring);
				st->audio_ring = NULL;
				audio_idle = 0;
			}
		}

		/* ── Continuous recording ── */
		bool rec = atomic_load(&st->recording);

		/* Handle stop transition */
		if (was_recording && !rec) {
			close_segment(st);
			was_recording = false;
			v_ts_base = -1;
			a_dts_counter = 0;
			a_ts_base = -1;
			RSS_INFO("recording stopped");
		}

		if (rec) {
			/* Continuous destination absent (standby above covers
			 * the nothing-writable case, so clips may still be
			 * live): skip without stalling — sleeping here starves
			 * the ring reader into overflow, and every overflow
			 * requests an IDR. */
			if (!rmr_storage_available(st->storage))
				goto clip_handling;

			/* Flush fragment periodically for steady write pattern.
			 * On keyframe: also check segment rotation. */
			if (st->mux) {
				st->frames_since_flush++;
				/* Ask the encoder for an IDR on the first frame
				 * past the wall-clock boundary so the split lands
				 * within a frame or two of :00 instead of a GOP
				 * late. Requesting BEFORE the boundary is a trap:
				 * the forced key arrives before the boundary too,
				 * cannot rotate, and restarts the GOP, pushing
				 * the first eligible key a full GOP past :00
				 * (constant +2s segments on 20fps HEVC). */
				int64_t now_rt = rss_wallclock_us();
				if (!st->segment_idr_requested && !meta.is_key &&
				    now_rt >= st->segment_boundary_rt_us) {
					rss_ring_request_idr(st->video_ring);
					st->segment_idr_requested = true;
				}
				if (meta.is_key) {
					if (rmr_mux_flush_fragment(st->mux) < 0)
						st->segment_write_error = true;
					if (st->sign_enabled && !st->segment_write_error)
						rmr_sign_stream_emit(&st->sign_seg, false,
								     direct_write, st);
					st->frames_since_flush = 0;
					if (st->segment_write_error) {
						/* The write stream broke (I/O
						 * error, NFS timeout): stop
						 * writing to this file (left in
						 * place). The normal flow below
						 * starts a fresh one at the next
						 * keyframe. */
						close_segment(st);
					} else if (rmr_storage_should_rotate_at(
							   st->storage, st->segment_start_rt_us,
							   now_rt)) {
						close_segment(st);
						rmr_storage_enforce_limit(st->storage);
					}
				} else if (st->frames_since_flush >= 10) {
					if (rmr_mux_flush_fragment(st->mux) < 0)
						st->segment_write_error = true;
					if (st->sign_enabled && !st->segment_write_error)
						rmr_sign_stream_emit(&st->sign_seg, false,
								     direct_write, st);
					st->frames_since_flush = 0;
					if (st->segment_write_error)
						close_segment(st);
				}
			}

			/* Start new segment if needed */
			if (!st->mux) {
				if (meta.is_key) {
					if (start_segment(st) < 0)
						goto clip_handling;
					v_ts_base = -1;
					a_dts_counter = 0;
					a_ts_base = -1;
					st->frames_written = 0;
					st->bytes_written = 0;
					was_recording = true;
				} else {
					goto clip_handling;
				}
			}

			/* DTS from ring timestamp */
			if (v_ts_base < 0)
				v_ts_base = meta.timestamp;
			int64_t v_dts = (meta.timestamp - v_ts_base) * 90 / 1000;

			rmr_video_sample_t vs = {
				.data = mux_data,
				.size = mux_len,
				.dts = v_dts,
				.pts = v_dts,
				.is_key = meta.is_key,
			};

			if (rmr_mux_write_video(st->mux, &vs) == 0)
				st->frames_written++;
			else
				st->frames_dropped++;

			/* Write audio to continuous mux */
			for (int i = 0; i < audio_count; i++) {
				a_dts_counter = steer_audio_dts(a_dts_counter, audio_frames[i].ts,
								&a_ts_base, audio_frames[i].samples,
								st->audio_sample_rate);
				rmr_audio_sample_t as = {
					.data = audio_frames[i].data,
					.size = audio_frames[i].len,
					.dts = a_dts_counter,
				};
				rmr_mux_write_audio(st->mux, &as);
				a_dts_counter += audio_frames[i].samples;
			}
		}

	clip_handling:
		/* ── Motion clip handling ── */
		if (st->mode == RMR_MODE_MOTION || st->mode == RMR_MODE_BOTH) {
			bool clip_want = (st->mode == RMR_MODE_BOTH)
						 ? atomic_load(&st->clip_recording)
						 : atomic_load(&st->recording);

			/* Start clip with pre-buffer */
			if (clip_want && !st->clip_mux && rmr_storage_available(st->clip_storage)) {
				if (open_clip_with_prebuffer(st, audio_samples_per_frame,
							     audio_bps) == 0) {
					RSS_INFO("motion clip active");
				}
			}

			/* Write live frames to clip */
			if (st->clip_mux) {
				clip_write_video(st, mux_data, mux_len, meta.timestamp,
						 meta.is_key);

				for (int i = 0; i < audio_count; i++)
					clip_write_audio(
						st, audio_frames[i].data, audio_frames[i].len,
						audio_frames[i].samples, audio_frames[i].ts);

				/* Check clip length cap */
				if (st->clip_length_sec > 0) {
					int64_t elapsed = rss_timestamp_us() - st->clip_start_us;
					if (elapsed >= (int64_t)st->clip_length_sec * 1000000) {
						close_clip(st);
						/* If still triggered, open a continuation
						 * clip (no pre-buffer on continuation) */
						if (clip_want) {
							if (open_clip(st) == 0) {
								RSS_INFO("clip rotated (length "
									 "cap)");
							}
						}
					}
				}
			}

			/* Stop clip */
			if (!clip_want && st->clip_mux)
				close_clip(st);
		}

		/* ── Timelapse sampling ── */
		if (st->tl_enabled && (st->params.ready || st->video_codec == 2)) {
			bool was_armed = st->tl.want_sample;
			rmr_tl_tick(&st->tl, rss_timestamp_us());
			/* The sample must be a keyframe; ask for one the
			 * moment a tick arms so the wait is one frame, not
			 * the rest of a GOP. Requests coalesce with the
			 * segment splitter's own. */
			if (!was_armed && st->tl.want_sample && st->video_codec != 2)
				rss_ring_request_idr(st->video_ring);

			bool tl_key = meta.is_key || st->video_codec == 2;
			if (tl_key && rmr_tl_take(&st->tl, st->video_read_seq))
				timelapse_write_sample(st, mux_data, mux_len);
		}
	}

	/* Shutdown */
	if (st->mux)
		close_segment(st);
	if (st->clip_mux)
		close_clip(st);
}

/* ── Entry point ── */

/* Compute next power of 2 >= n */
static uint32_t next_pow2(uint32_t n)
{
	if (n == 0)
		return 1;
	n--;
	n |= n >> 1;
	n |= n >> 2;
	n |= n >> 4;
	n |= n >> 8;
	n |= n >> 16;
	return n + 1;
}

int main(int argc, char **argv)
{
	rss_daemon_ctx_t dctx;
	int ret = rss_daemon_init(&dctx, "rmr", argc, argv, NULL);
	if (ret != 0)
		return ret < 0 ? 1 : 0;
	bool rec_enabled = rss_config_get_bool(dctx.cfg, "recording", "enabled", false);
	bool tl_enabled = rss_config_get_bool(dctx.cfg, "timelapse", "enabled", false);
	if (!rec_enabled && !tl_enabled) {
		RSS_INFO("recording and timelapse disabled in config");
		rss_config_free(dctx.cfg);
		rss_daemon_cleanup("rmr");
		return 0;
	}

	rmr_state_t st = {0};
	st.cfg = dctx.cfg;
	st.config_path = dctx.config_path;
	st.running = dctx.running;
	st.segment_fd = -1;
	st.clip_fd = -1;
	st.tl_fd = -1;
	st.tl_enabled = tl_enabled;
	rmr_tl_init(&st.tl, rss_config_get_int(dctx.cfg, "timelapse", "interval", 10),
		    rss_config_get_int(dctx.cfg, "timelapse", "playback_fps", 30),
		    rss_config_get_int(dctx.cfg, "timelapse", "file_frames", 0));
	st.stream_idx = rss_config_get_int(dctx.cfg, "recording", "stream", 0);
	st.audio_enabled = rss_config_get_bool(dctx.cfg, "recording", "audio", true);
	st.sei_timecode = rss_config_get_bool(dctx.cfg, "recording", "sei_timecode", true);
	st.sign_enabled = rss_config_get_bool(dctx.cfg, "recording", "sign", true);
	if (st.sign_enabled) {
		const char *key_path = rss_config_get_str(dctx.cfg, "recording", "sign_key",
							  "/etc/raptor/sign_ed25519.key");
		if (rss_sign_key_load(&st.sign_key, key_path) < 0) {
			RSS_ERROR("signing disabled: key unavailable");
			st.sign_enabled = false;
		}
	}

	/* Parse recording mode */
	const char *mode_str = rss_config_get_str(dctx.cfg, "recording", "mode", "continuous");
	if (strcmp(mode_str, "motion") == 0)
		st.mode = RMR_MODE_MOTION;
	else if (strcmp(mode_str, "both") == 0)
		st.mode = RMR_MODE_BOTH;
	else
		st.mode = RMR_MODE_CONTINUOUS;

	st.prebuffer_sec = rss_config_get_int(dctx.cfg, "recording", "prebuffer_sec", 5);
	if (st.prebuffer_sec < 0)
		st.prebuffer_sec = 0;
	if (st.prebuffer_sec > 5)
		st.prebuffer_sec = 5;

	st.clip_length_sec = rss_config_get_int(dctx.cfg, "recording", "clip_length_sec", 60);

	/* Open video ring (stream 0-5 for multi-sensor) */
	static const char *ring_names[] = {"main",   "sub",   "s1_main",  "s1_sub",  "s2_main",
					   "s2_sub", "jpeg0", "s1_jpeg0", "s2_jpeg0"};
	int ri = st.stream_idx;
	if (ri < 0 || ri >= (int)(sizeof(ring_names) / sizeof(ring_names[0])))
		ri = 0;
	const char *ring_name = ring_names[ri];
	st.video_ring_name = ring_name;

	for (int attempt = 0; attempt < 30 && *st.running; attempt++) {
		st.video_ring = rss_ring_open(st.video_ring_name);
		if (st.video_ring)
			break;
		RSS_DEBUG("waiting for %s ring...", ring_name);
		sleep(1);
	}

	if (!st.video_ring) {
		RSS_FATAL("video ring not available");
		goto cleanup;
	}
	rss_ring_check_version(st.video_ring, "video");
	rss_ring_acquire(st.video_ring);

	/* Read ring metadata */
	const rss_ring_header_t *vhdr = rss_ring_get_header(st.video_ring);
	st.video_codec = vhdr->codec;
	st.width = vhdr->width;
	st.height = vhdr->height;
	st.fps_num = vhdr->fps_num;
	const char *vcodec_name = st.video_codec == 2	? "MJPEG"
				  : st.video_codec == 1 ? "H.265"
							: "H.264";
	RSS_INFO("video: %s %ux%u @ %u fps", vcodec_name, st.width, st.height, st.fps_num);

	/* Allocate buffers (AVCC gets headroom for the leading SEI NAL) */
	st.frame_buf_size = rss_ring_max_frame_size(st.video_ring);
	st.frame_buf = malloc(st.frame_buf_size);
	st.avcc_buf_size = st.frame_buf_size + RSS_SEI_TS_MAX;
	st.avcc_buf = malloc(st.avcc_buf_size);
	if (!st.frame_buf || !st.avcc_buf) {
		RSS_FATAL("buffer allocation failed (%u bytes)", st.frame_buf_size);
		goto cleanup;
	}

	/* Open audio ring */
	if (st.audio_enabled) {
		st.audio_ring = rss_ring_open("audio");
		if (st.audio_ring) {
			rss_ring_check_version(st.audio_ring, "audio");
			const rss_ring_header_t *ahdr = rss_ring_get_header(st.audio_ring);
			st.audio_codec = ahdr->codec;
			st.audio_sample_rate = ahdr->fps_num;
			st.audio_aot = ahdr->profile;
			RSS_DEBUG("audio: codec=%u rate=%u aot=%u", st.audio_codec,
				  st.audio_sample_rate, st.audio_aot);
		} else {
			RSS_WARN("audio ring not available (recording video only)");
		}
	}

	/* Create pre-buffers for motion clip modes */
	if (st.prebuffer_sec > 0 && (st.mode == RMR_MODE_MOTION || st.mode == RMR_MODE_BOTH)) {
		uint32_t fps = st.fps_num > 0 ? st.fps_num : 25;
		uint32_t v_frames = fps * (uint32_t)st.prebuffer_sec + fps; /* +1s margin */
		uint32_t v_slots = next_pow2(v_frames);
		/* Data size: bitrate * prebuffer_sec * 2.5 (headroom for I-frames) / 8 */
		uint32_t bps = rss_config_get_int(dctx.cfg, "stream0", "bitrate", 2000000);
		uint64_t v_data = (uint64_t)bps * (uint32_t)st.prebuffer_sec * 5 / 2 / 8;
		if (v_data < 1024 * 1024)
			v_data = 1024 * 1024; /* min 1MB */
		if (v_data > 10 * 1024 * 1024)
			v_data = 10 * 1024 * 1024; /* max 10MB */

		st.video_pb = rmr_prebuf_create(v_slots, (uint32_t)v_data);
		if (st.video_pb)
			RSS_DEBUG("video pre-buffer: %u slots, %u KB data", v_slots,
				  (uint32_t)(v_data / 1024));
		else
			RSS_WARN("video pre-buffer alloc failed");

		if (st.audio_ring) {
			/* Audio: 50fps (20ms frames) typical */
			uint32_t a_frames = 50 * (uint32_t)st.prebuffer_sec + 50;
			uint32_t a_slots = next_pow2(a_frames);
			/* Audio data: small frames, 1KB each is generous */
			uint32_t a_data = a_slots * 1024;
			if (a_data < 128 * 1024)
				a_data = 128 * 1024;

			st.audio_pb = rmr_prebuf_create(a_slots, a_data);
			if (st.audio_pb)
				RSS_DEBUG("audio pre-buffer: %u slots, %u KB data", a_slots,
					  a_data / 1024);
			else
				RSS_WARN("audio pre-buffer alloc failed");
		}
	}

	/* Storage */
	rmr_storage_config_t scfg = {
		.base_path = rss_config_get_str(dctx.cfg, "recording", "storage_path",
						"/mnt/mmcblk0p1/raptor"),
		.segment_minutes = rss_config_get_int(dctx.cfg, "recording", "segment_minutes", 5),
		.segment_seconds = rss_config_get_int(dctx.cfg, "recording", "segment_seconds", 0),
		.max_storage_mb = rss_config_get_int(dctx.cfg, "recording", "max_storage_mb", 0),
	};
	st.storage = rmr_storage_create(&scfg);
	if (!st.storage) {
		RSS_FATAL("storage init failed");
		goto cleanup;
	}

	/* Control socket */
	rss_mkdir_p(RSS_RUN_DIR);
	st.ctrl = rss_ctrl_listen(RSS_RUN_DIR "/rmr.sock");
	if (!st.ctrl)
		RSS_WARN("control socket failed (non-fatal)");

	/* Set up clip storage for motion modes */
	if (st.mode == RMR_MODE_BOTH || st.mode == RMR_MODE_MOTION) {
		char clip_path[280];
		snprintf(clip_path, sizeof(clip_path), "%s/clips",
			 rss_config_get_str(dctx.cfg, "recording", "storage_path",
					    "/mnt/mmcblk0p1/raptor"));
		rmr_storage_config_t ccfg = {
			.base_path = clip_path,
			.segment_minutes = (st.clip_length_sec + 59) / 60,
			.max_storage_mb =
				rss_config_get_int(dctx.cfg, "recording", "clip_max_mb", 100),
		};
		if (ccfg.segment_minutes < 1)
			ccfg.segment_minutes = 1;
		st.clip_storage = rmr_storage_create(&ccfg);
		if (!st.clip_storage)
			RSS_WARN("clip storage init failed — motion clips disabled");
	}

	/* Timelapse storage: same proven shape as clips — its own
	 * subdirectory and its own quota, invisible to the main scan. */
	{
		char tl_path[280];
		snprintf(tl_path, sizeof(tl_path), "%s/timelapse",
			 rss_config_get_str(dctx.cfg, "recording", "storage_path",
					    "/mnt/mmcblk0p1/raptor"));
		rmr_storage_config_t tcfg = {
			.base_path = tl_path,
			.segment_minutes = 24 * 60,
			.max_storage_mb = rss_config_get_int(dctx.cfg, "timelapse", "max_mb", 2048),
		};
		st.tl_storage = rmr_storage_create(&tcfg);
		if (!st.tl_storage)
			RSS_WARN("timelapse storage init failed — timelapse disabled");
	}

	/* Start continuous recording for 'continuous' and 'both' modes,
	 * but never when only the timelapse enabled this daemon. */
	if (!rec_enabled)
		RSS_INFO("timelapse only — recording stays off");
	else if (st.mode != RMR_MODE_MOTION)
		atomic_store(&st.recording, true);
	else
		RSS_INFO("mode=motion — waiting for trigger");

	/* Run main loop */
	record_loop(&st);

	RSS_INFO("rmr shutting down");

cleanup:
	if (st.ctrl)
		rss_ctrl_destroy(st.ctrl);
	if (st.clip_mux)
		close_clip(&st);
	if (st.tl_mux)
		close_timelapse(&st);
	if (st.tl_storage)
		rmr_storage_destroy(st.tl_storage);
	if (st.clip_storage)
		rmr_storage_destroy(st.clip_storage);
	if (st.storage)
		rmr_storage_destroy(st.storage);
	if (st.video_ring) {
		rss_ring_release(st.video_ring);
		rss_ring_close(st.video_ring);
	}
	if (st.audio_ring)
		rss_ring_close(st.audio_ring);
	if (st.video_pb)
		rmr_prebuf_destroy(st.video_pb);
	if (st.audio_pb)
		rmr_prebuf_destroy(st.audio_pb);
	free(st.frame_buf);
	free(st.avcc_buf);
	rss_config_free(dctx.cfg);

	rss_daemon_cleanup("rmr");

	return 0;
}

/*
 * rmr_main.c -- Raptor Media Recorder
 *
 * Reads H.264/H.265 video + audio from SHM rings and writes
 * fragmented MP4 files to SD card. Single-threaded: the main loop
 * reads rings, feeds the muxers, and queues file writes to the
 * writer thread.
 *
 * Several video rings can be recorded at once (a dashcam's front and
 * rear cameras): each is a track with its own files, named after the
 * first track's plus a tag. Audio, telematics, events and storage are
 * shared by all tracks.
 *
 * Motion clips with pre-buffer: a process-local circular buffer per
 * track stores the last N seconds of frames. When motion triggers, the
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

#define RING_RETRY_US	  200000
#define RING_IDLE_US	  2000000
#define AUDIO_RETRY_US	  2000000
#define RATE_WINDOW_US	  3000000
#define TRACK_READ_BURST  8
#define AUDIO_READ_BURST  16
#define LOOP_WAIT_MS	  10
#define PREBUF_START_SIZE (1024 * 1024)

static bool is_primary(const rmr_track_t *t)
{
	return t == &t->st->tracks[0];
}

/* ── Write callbacks ── */

/* Bytes are counted as they queue: the counters are file offsets (GPS
 * records index by them), and the writer thread keeps file order. */
static int queue_write(rmr_state_t *st, int fd, const void *buf, uint32_t len, uint64_t *off,
		       rmr_sign_stream_t *sign)
{
	if (len == 0)
		return 0;
	if (rmr_io_write(st->io, fd, buf, len) < 0)
		return -1;
	*off += len;
	if (st->sign_enabled)
		rmr_sign_stream_update(sign, buf, len);
	return 0;
}

static int direct_write(const void *buf, uint32_t len, void *ctx)
{
	rmr_track_t *t = ctx;
	uint64_t before = t->segment_bytes;
	int ret = queue_write(t->st, t->segment_fd, buf, len, &t->segment_bytes, &t->sign_seg);
	t->bytes_written += t->segment_bytes - before;
	return ret;
}

static int clip_write(const void *buf, uint32_t len, void *ctx)
{
	rmr_track_t *t = ctx;
	return queue_write(t->st, t->clip_fd, buf, len, &t->clip_bytes, &t->sign_clip);
}

static int tl_write(const void *buf, uint32_t len, void *ctx)
{
	rmr_state_t *st = ctx;
	return queue_write(st, st->tl_fd, buf, len, &st->tl_bytes, &st->sign_tl);
}

/* ── GPS records ── */

#define TELE_RETRY_US 5000000

static void gps_write(rmr_mux_t *mux, rmr_write_fn fn, uint64_t *file_off, void *ctx,
		      const rtd_record_t *r)
{
	uint8_t box[RMR_GPS_BOX_SIZE];
	rmr_gps_build(box, r);
	uint64_t off = *file_off;
	if (fn(box, sizeof(box), ctx) == 0)
		rmr_mux_gps_add(mux, off);
}

/* Fill the moov index, queued behind the records it names. Not hashed:
 * the signature chain covers the index as written (zeroed). Repeated at
 * every sync so a file cut by power loss keeps an index up to then. */
static void gps_patch_index(rmr_state_t *st, rmr_mux_t *mux, int fd)
{
	uint8_t *buf;
	uint32_t len;
	uint64_t off;
	if (rmr_mux_gps_index(mux, RMR_GPS_BOX_SIZE, &buf, &len, &off) < 0)
		return;
	rmr_io_pwrite(st->io, fd, buf, len, off);
	free(buf);
}

static uint32_t gps_capacity(int sec)
{
	if (sec <= 0 || sec > 3600)
		sec = 3600;
	return (uint32_t)sec + 120; /* boundary waits for an IDR, pre-roll */
}

static void telematics_poll(rmr_state_t *st)
{
	if (!st->gps)
		return;
	int64_t now = rss_timestamp_us();
	if (st->tele_ring && rss_ring_stale(st->tele_ring)) {
		rss_ring_close(st->tele_ring);
		st->tele_ring = NULL;
	}
	if (!st->tele_ring) {
		if (now < st->tele_retry_us)
			return;
		st->tele_retry_us = now + TELE_RETRY_US;
		st->tele_ring = rss_ring_open(RTD_RING);
		if (!st->tele_ring)
			return;
		st->tele_read_seq = atomic_load(&rss_ring_get_header(st->tele_ring)->write_seq);
		RSS_INFO("telematics ring attached");
	}

	rtd_record_t r;
	uint32_t len;
	rss_ring_slot_t meta;
	int ret;
	while ((ret = rss_ring_read(st->tele_ring, &st->tele_read_seq, (uint8_t *)&r, sizeof(r),
				    &len, &meta)) == 0 ||
	       ret == RSS_EOVERFLOW) {
		if (ret == RSS_EOVERFLOW || len != sizeof(r) || r.version != RTD_REC_VERSION)
			continue;
		st->tele_hist[st->tele_hist_n++ % 8] = r;
		for (int i = 0; i < st->track_count; i++) {
			rmr_track_t *t = &st->tracks[i];
			if (t->mux)
				gps_write(t->mux, direct_write, &t->segment_bytes, t, &r);
			if (t->clip_mux)
				gps_write(t->clip_mux, clip_write, &t->clip_bytes, t, &r);
		}
	}
}

/* ── Segment management ── */

static void setup_mux_video_track_dur(rmr_mux_t *mux, const rmr_track_t *t,
				      uint32_t default_duration)
{
	rmr_video_params_t vp = {
		.codec = (t->codec == 2)   ? RMR_CODEC_MJPEG
			 : (t->codec == 1) ? RMR_CODEC_H265
					   : RMR_CODEC_H264,
		.width = t->width,
		.height = t->height,
		.timescale = 90000,
		.default_duration = default_duration,
	};
	if (t->codec == 2)
		rmr_mux_set_video(mux, &vp, NULL, 0, NULL, 0, NULL, 0);
	else
		rmr_mux_set_video(mux, &vp, t->params.sps, t->params.sps_len, t->params.pps,
				  t->params.pps_len, t->params.vps_len > 0 ? t->params.vps : NULL,
				  t->params.vps_len);
}

static void setup_mux_tracks(rmr_mux_t *mux, const rmr_track_t *t)
{
	const rmr_state_t *st = t->st;
	setup_mux_video_track_dur(mux, t, 0);

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

static int start_segment(rmr_track_t *t)
{
	rmr_state_t *st = t->st;
	int seg_len = rmr_storage_segment_len_sec(st->storage);
	int64_t now_rt = rss_wallclock_us();
	int64_t name_rt = rmr_name_stamp_take(&st->segment_name, &t->segment_stamp, now_rt, now_rt,
					      rmr_storage_next_boundary(now_rt, seg_len));
	int fd = rmr_storage_open_file(st->storage, name_rt, t->tag, t->segment_path,
				       sizeof(t->segment_path));
	if (fd < 0)
		return -1;

	t->mux = rmr_mux_create(direct_write, t);
	if (!t->mux) {
		rmr_storage_close_segment(fd);
		return -1;
	}

	setup_mux_tracks(t->mux, t);
	if (st->gps)
		rmr_mux_enable_gps(t->mux, gps_capacity(seg_len));
	t->segment_fd = fd;
	t->segment_bytes = 0;
	if (st->sign_enabled)
		rmr_sign_stream_begin(&t->sign_seg, &st->sign_key);
	rmr_mux_start(t->mux);
	if (st->sign_enabled)
		rmr_sign_stream_emit(&t->sign_seg, false, direct_write, t);
	t->segment_start_rt_us = now_rt;
	t->segment_boundary_rt_us = rmr_storage_next_boundary(now_rt, seg_len);
	t->segment_idr_requested = false;

	RSS_INFO("recording segment: %s", t->segment_path);
	return 0;
}

static void close_segment(rmr_track_t *t)
{
	rmr_state_t *st = t->st;
	if (t->mux) {
		rmr_mux_finalize(t->mux);
		if (st->sign_enabled)
			rmr_sign_stream_emit(&t->sign_seg, true, direct_write, t);
		if (t->segment_fd >= 0)
			gps_patch_index(st, t->mux, t->segment_fd);
		rmr_mux_destroy(t->mux);
		t->mux = NULL;
	}

	int fd = t->segment_fd;
	t->segment_fd = -1;

	if (fd >= 0) {
		rmr_io_close(st->io, fd);
		RSS_DEBUG("segment closed: %s (%" PRIu64 " frames, %" PRIu64 " bytes)",
			  t->segment_path, t->frames_written, t->bytes_written);
	}
}

/* ── Motion clip management ── */

static int open_clip(rmr_track_t *t)
{
	rmr_state_t *st = t->st;
	if (!st->clip_storage)
		return -1;

	int64_t now = rss_timestamp_us();
	int64_t until =
		st->clip_length_sec > 0 ? now + (int64_t)st->clip_length_sec * 1000000 : INT64_MAX;
	int64_t name_rt =
		rmr_name_stamp_take(&st->clip_name, &t->clip_stamp, now, rss_wallclock_us(), until);
	int fd = rmr_storage_open_file(st->clip_storage, name_rt, t->tag, t->clip_path,
				       sizeof(t->clip_path));
	if (fd < 0)
		return -1;

	t->clip_mux = rmr_mux_create(clip_write, t);
	if (!t->clip_mux) {
		rmr_storage_close_segment(fd);
		return -1;
	}

	t->clip_fd = fd;
	t->clip_v_ts_base = -1;
	t->clip_a_dts = 0;
	t->clip_a_ts_base = -1;
	t->clip_start_us = now;
	t->clip_bytes = 0;

	setup_mux_tracks(t->clip_mux, t);
	if (st->gps)
		rmr_mux_enable_gps(t->clip_mux,
				   gps_capacity(st->clip_length_sec + st->prebuffer_sec));
	if (st->sign_enabled)
		rmr_sign_stream_begin(&t->sign_clip, &st->sign_key);
	rmr_mux_start(t->clip_mux);
	if (st->sign_enabled)
		rmr_sign_stream_emit(&t->sign_clip, false, clip_write, t);

	int64_t since = t->clip_start_us - ((int64_t)st->prebuffer_sec + 1) * 1000000;
	unsigned n = st->tele_hist_n < 8 ? st->tele_hist_n : 8;
	for (unsigned i = st->tele_hist_n - n; i < st->tele_hist_n; i++)
		if (st->tele_hist[i % 8].ts_us >= since)
			gps_write(t->clip_mux, clip_write, &t->clip_bytes, t,
				  &st->tele_hist[i % 8]);

	RSS_INFO("motion clip started: %s", t->clip_path);
	return 0;
}

static void close_clip(rmr_track_t *t)
{
	rmr_state_t *st = t->st;
	if (t->clip_mux) {
		rmr_mux_finalize(t->clip_mux);
		if (st->sign_enabled)
			rmr_sign_stream_emit(&t->sign_clip, true, clip_write, t);
		if (t->clip_fd >= 0)
			gps_patch_index(st, t->clip_mux, t->clip_fd);
		rmr_mux_destroy(t->clip_mux);
		t->clip_mux = NULL;
	}
	if (t->clip_fd >= 0) {
		rmr_io_close(st->io, t->clip_fd);
		RSS_DEBUG("motion clip closed: %s (%" PRIu64 " bytes)", t->clip_path,
			  t->clip_bytes);
		t->clip_fd = -1;
	}
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
		rmr_io_close(st->io, st->tl_fd);
		RSS_INFO("timelapse file closed: %s (%u frames, %" PRIu64 " bytes)", st->tl_path,
			 st->tl.frames_in_file, st->tl_bytes);
		st->tl_fd = -1;
	}
	st->tl.file_day = 0;
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

	/* Fragment-per-sample: without a next sample to derive from, each
	 * sample's declared duration must be the playback grid step or the
	 * file contradicts its own DTS grid and players drop frames. */
	setup_mux_video_track_dur(st->tl_mux, &st->tracks[0], 90000 / st->tl.playback_fps);
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
	rmr_mux_flush_fragment(st->tl_mux);
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
static void clip_write_video(rmr_track_t *t, const uint8_t *avcc, uint32_t avcc_len,
			     int64_t timestamp, bool is_key)
{
	if (!t->clip_mux)
		return;

	if (t->clip_v_ts_base < 0)
		t->clip_v_ts_base = timestamp;

	int64_t v_dts = (timestamp - t->clip_v_ts_base) * 90 / 1000;
	rmr_video_sample_t vs = {
		.data = avcc,
		.size = avcc_len,
		.dts = v_dts,
		.pts = v_dts,
		.is_key = is_key,
	};

	if (is_key) {
		rmr_mux_flush_fragment(t->clip_mux);
		if (t->st->sign_enabled)
			rmr_sign_stream_emit(&t->sign_clip, false, clip_write, t);
	}
	rmr_mux_write_video(t->clip_mux, &vs);
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
static void clip_write_audio(rmr_track_t *t, const uint8_t *data, uint32_t len, uint32_t samples,
			     int64_t ring_ts)
{
	if (!t->clip_mux)
		return;

	t->clip_a_dts = steer_audio_dts(t->clip_a_dts, ring_ts, &t->clip_a_ts_base, samples,
					t->st->audio_sample_rate);
	rmr_audio_sample_t as = {
		.data = data,
		.size = len,
		.dts = t->clip_a_dts,
	};
	rmr_mux_write_audio(t->clip_mux, &as);
	t->clip_a_dts += samples;
}

/* ── Pre-buffer replay ── */

typedef struct {
	rmr_track_t *t;
	int max_frames; /* limit replay count, -1 = unlimited */
	int count;	/* frames replayed so far */
} replay_ctx_t;

static uint32_t audio_frame_samples(const rmr_state_t *st, uint32_t len)
{
	return st->audio_samples_per_frame ? st->audio_samples_per_frame : len / st->audio_bps;
}

static int replay_video_frame(const rmr_prebuf_slot_t *slot, const uint8_t *data, void *ctx)
{
	replay_ctx_t *rc = ctx;
	clip_write_video(rc->t, data, slot->data_length, slot->timestamp, slot->is_key);
	rc->count++;
	return 0;
}

static int replay_audio_frame(const rmr_prebuf_slot_t *slot, const uint8_t *data, void *ctx)
{
	replay_ctx_t *rc = ctx;
	if (rc->max_frames >= 0 && rc->count >= rc->max_frames)
		return 1; /* stop iteration */
	clip_write_audio(rc->t, data, slot->data_length,
			 audio_frame_samples(rc->t->st, slot->data_length), slot->timestamp);
	rc->count++;
	return 0;
}

static int open_clip_with_prebuffer(rmr_track_t *t)
{
	rmr_state_t *st = t->st;
	if (open_clip(t) < 0)
		return -1;

	/* Search one extra second back so the keyframe-aligned pre-buffer
	 * always meets or exceeds the configured duration. */
	int64_t max_age_us = ((int64_t)st->prebuffer_sec + 1) * 1000000;

	/* Find the oldest keyframe within the pre-buffer window */
	uint32_t vstart = rmr_prebuf_find_keyframe(t->video_pb, max_age_us);
	if (vstart == UINT32_MAX) {
		RSS_WARN("no keyframe in pre-buffer, clip starts without pre-buffer");
		return 0;
	}

	/* Get the video pre-buffer time range */
	int64_t kf_ts = rmr_prebuf_timestamp(t->video_pb, vstart);
	int64_t newest_v_ts = rmr_prebuf_timestamp(t->video_pb, rmr_prebuf_newest(t->video_pb));
	int64_t v_duration_us = newest_v_ts - kf_ts;

	replay_ctx_t rc = {
		.t = t,
		.max_frames = -1,
		.count = 0,
	};

	/* Replay video pre-buffer */
	int vcount = rmr_prebuf_iterate(t->video_pb, vstart, replay_video_frame, &rc);
	RSS_DEBUG("pre-buffer: replayed %d video frames (%.1fs)", vcount,
		  v_duration_us / 1000000.0);

	/* Replay audio pre-buffer — match video duration by frame count.
	 * Timestamp matching is unreliable across rings, so calculate
	 * how many audio frames fit in the video pre-buffer duration. */
	if (st->audio_pb && rmr_prebuf_count(st->audio_pb) > 0 && v_duration_us > 0) {
		/* Audio frame duration in microseconds */
		int64_t audio_frame_us;
		if (st->audio_samples_per_frame > 0 && st->audio_sample_rate > 0)
			audio_frame_us = (int64_t)st->audio_samples_per_frame * 1000000 /
					 st->audio_sample_rate;
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

static cJSON *tracks_json(const rmr_state_t *st)
{
	cJSON *arr = cJSON_CreateArray();
	if (!arr)
		return NULL;
	for (int i = 0; i < st->track_count; i++) {
		const rmr_track_t *t = &st->tracks[i];
		cJSON *o = cJSON_CreateObject();
		if (!o) {
			cJSON_Delete(arr);
			return NULL;
		}
		cJSON_AddStringToObject(o, "ring", t->ring_name);
		cJSON_AddStringToObject(o, "tag", t->tag);
		cJSON_AddBoolToObject(o, "active", t->ring && !t->idle);
		cJSON_AddStringToObject(o, "file", t->segment_path);
		cJSON_AddNumberToObject(o, "frames", (double)t->frames_written);
		cJSON_AddNumberToObject(o, "dropped", (double)t->frames_dropped);
		cJSON_AddNumberToObject(o, "bytes", (double)t->bytes_written);
		cJSON_AddNumberToObject(o, "frames_lost", (double)t->frames_lost);
		cJSON_AddStringToObject(o, "clip", t->clip_mux ? t->clip_path : "");
		cJSON_AddItemToArray(arr, o);
	}
	return arr;
}

static int rmr_ctrl_handler(const char *cmd_json, char *resp_buf, int resp_buf_size, void *userdata)
{
	rmr_state_t *st = userdata;
	rmr_track_t *t0 = &st->tracks[0];

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

	/* Continuous recording only: clips still start on events (a parked
	 * dashcam records impacts, not the empty car park). */
	if (strcmp(cmd, "pause") == 0) {
		atomic_store(&st->recording, false);
		return rss_ctrl_resp_ok(resp_buf, resp_buf_size);
	}

	if (strcmp(cmd, "resume") == 0) {
		atomic_store(&st->recording, true);
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
		if (!r)
			return rss_ctrl_resp_error(resp_buf, resp_buf_size, "alloc");
		cJSON_AddBoolToObject(r, "recording", atomic_load(&st->recording));
		cJSON_AddBoolToObject(r, "clip", atomic_load(&st->clip_recording));
		cJSON_AddStringToObject(r, "mode", mode_names[st->mode]);
		cJSON_AddStringToObject(r, "storage_path", rmr_storage_path(st->storage));
		cJSON_AddStringToObject(r, "file", t0->segment_path);
		cJSON_AddNumberToObject(r, "frames", (double)t0->frames_written);
		cJSON_AddNumberToObject(r, "dropped", (double)t0->frames_dropped);
		cJSON_AddNumberToObject(r, "bytes", (double)t0->bytes_written);
		cJSON_AddBoolToObject(r, "sign", st->sign_enabled);
		cJSON_AddBoolToObject(r, "sei_timecode", st->sei_timecode);
		cJSON_AddBoolToObject(r, "gps", st->tele_ring != NULL);
		cJSON_AddNumberToObject(r, "frames_lost", (double)t0->frames_lost);
		cJSON_AddNumberToObject(r, "io_pending_kb", rmr_io_pending(st->io) / 1024);
		cJSON_AddNumberToObject(r, "io_peak_kb", rmr_io_peak(st->io) / 1024);
		cJSON_AddBoolToObject(r, "timelapse", st->tl_enabled);
		cJSON *tracks = tracks_json(st);
		if (!tracks) {
			cJSON_Delete(r);
			return rss_ctrl_resp_error(resp_buf, resp_buf_size, "alloc");
		}
		cJSON_AddItemToObject(r, "tracks", tracks);
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
		if (t0->ring && t0->codec != 2)
			rss_ring_request_idr(t0->ring);
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

/* Writer-thread errors surface here; a failed file is closed (what
 * reached the medium stays playable) and the next keyframe opens a new
 * one unless the storage is full. Then the periodic sync: the GPS index
 * first, so the sync makes both durable together. */
static void io_service(rmr_state_t *st)
{
	int err;
	for (int i = 0; i < st->track_count; i++) {
		rmr_track_t *t = &st->tracks[i];
		if (t->segment_fd >= 0 && (err = rmr_io_error(st->io, t->segment_fd)) != 0) {
			RSS_ERROR("recording write failed: %s", strerror(err));
			if (err == ENOSPC && rmr_storage_enforce_limit(st->storage) == 0) {
				RSS_ERROR("storage full, stopping recording");
				atomic_store(&st->recording, false);
			}
			close_segment(t);
		}
		if (t->clip_fd >= 0 && (err = rmr_io_error(st->io, t->clip_fd)) != 0) {
			RSS_ERROR("clip write failed: %s", strerror(err));
			close_clip(t);
		}
	}
	if (st->tl_fd >= 0 && (err = rmr_io_error(st->io, st->tl_fd)) != 0) {
		RSS_ERROR("timelapse write failed: %s", strerror(err));
		close_timelapse(st);
	}

	if (st->sync_interval_ms <= 0)
		return;
	int64_t now = rss_timestamp_us();
	if (now < st->next_sync_us)
		return;
	st->next_sync_us = now + (int64_t)st->sync_interval_ms * 1000;
	for (int i = 0; i < st->track_count; i++) {
		rmr_track_t *t = &st->tracks[i];
		if (t->mux && t->segment_fd >= 0) {
			gps_patch_index(st, t->mux, t->segment_fd);
			rmr_io_sync(st->io, t->segment_fd);
		}
		if (t->clip_mux && t->clip_fd >= 0) {
			gps_patch_index(st, t->clip_mux, t->clip_fd);
			rmr_io_sync(st->io, t->clip_fd);
		}
	}
}

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

/* Slots from the ring's frame rate; the data region starts small and is
 * sized once from the observed byte rate after RATE_WINDOW_US, rather
 * than doubling its way there (each doubling briefly holds both). */
static void track_prebuf_init(rmr_state_t *st, rmr_track_t *t, uint32_t fps)
{
	if (t->video_pb || st->prebuffer_sec <= 0 || st->mode == RMR_MODE_CONTINUOUS)
		return;
	/* Slots are cheap; a producer that has not published its stream
	 * info yet (RCD before the first keyframe) must not cut the window */
	if (fps == 0 || fps > 120)
		fps = 60;
	uint32_t v_slots = next_pow2(fps * ((uint32_t)st->prebuffer_sec + 1));
	uint32_t v_data = PREBUF_START_SIZE < st->prebuffer_max_bytes ? PREBUF_START_SIZE
								      : st->prebuffer_max_bytes;
	t->video_pb = rmr_prebuf_create(v_slots, v_data);
	if (!t->video_pb) {
		RSS_WARN("%s: video pre-buffer alloc failed", t->ring_name);
		return;
	}
	/* Room for a large I frame until the rate is known */
	rmr_prebuf_set_growth(t->video_pb,
			      v_data * 2 < st->prebuffer_max_bytes ? v_data * 2 : v_data,
			      ((int64_t)st->prebuffer_sec + 1) * 1000000);
	t->rate_start_us = -1;
	t->rate_bytes = 0;
	t->rate_sized = false;
	RSS_DEBUG("%s: video pre-buffer: %u slots, %u KB data", t->ring_name, v_slots,
		  v_data / 1024);
}

static void track_prebuf_measure(rmr_state_t *st, rmr_track_t *t, uint32_t len, int64_t ts)
{
	if (!t->video_pb || t->rate_sized)
		return;
	if (t->rate_start_us < 0)
		t->rate_start_us = ts;
	t->rate_bytes += len;
	int64_t span = ts - t->rate_start_us;
	if (span < RATE_WINDOW_US)
		return;
	uint64_t bps = t->rate_bytes * 1000000 / (uint64_t)span;
	uint32_t size = rmr_prebuf_size_for_rate(bps, st->prebuffer_sec, st->prebuffer_max_bytes);
	rmr_prebuf_set_growth(t->video_pb, st->prebuffer_max_bytes,
			      ((int64_t)st->prebuffer_sec + 1) * 1000000);
	if (rmr_prebuf_reserve(t->video_pb, size) < 0)
		RSS_WARN("%s: video pre-buffer stays at %u KB", t->ring_name,
			 t->video_pb->data_size / 1024);
	t->rate_sized = true;
	RSS_INFO("%s: video pre-buffer %u KB for %" PRIu64 " kbit/s", t->ring_name,
		 t->video_pb->data_size / 1024, bps * 8 / 1000);
}

static void track_detach(rmr_track_t *t)
{
	if (!t->ring)
		return;
	rss_ring_release(t->ring);
	rss_ring_close(t->ring);
	t->ring = NULL;
}

static int track_attach(rmr_state_t *st, rmr_track_t *t)
{
	t->ring = rss_ring_open(t->ring_name);
	if (!t->ring)
		return -1;
	rss_ring_check_version(t->ring, t->ring_name);
	const rss_ring_header_t *hdr = rss_ring_get_header(t->ring);
	/* rcd creates its rings before the first keyframe tells it the
	 * stream info; attaching then would fix the track at 0x0. */
	if (hdr->width == 0 || hdr->height == 0) {
		rss_ring_close(t->ring);
		t->ring = NULL;
		return -1;
	}
	uint32_t mfs = rss_ring_max_frame_size(t->ring);
	if (mfs > t->frame_buf_size) {
		/* AVCC gets headroom for the leading SEI NAL */
		uint8_t *new_frame = malloc(mfs);
		uint8_t *new_avcc = malloc(mfs + RSS_SEI_TS_MAX);
		if (!new_frame || !new_avcc) {
			RSS_ERROR("%s: buffer allocation failed (%u bytes)", t->ring_name, mfs);
			free(new_frame);
			free(new_avcc);
			rss_ring_close(t->ring);
			t->ring = NULL;
			return -1;
		}
		free(t->frame_buf);
		free(t->avcc_buf);
		t->frame_buf = new_frame;
		t->avcc_buf = new_avcc;
		t->frame_buf_size = mfs;
		t->avcc_buf_size = mfs + RSS_SEI_TS_MAX;
	}

	/* A file's sample description is fixed at open: a source that came
	 * back with another codec or size starts new files. */
	if (t->attached_once &&
	    (hdr->codec != t->codec || hdr->width != t->width || hdr->height != t->height)) {
		if (t->mux)
			close_segment(t);
		if (t->clip_mux)
			close_clip(t);
	}
	t->codec = hdr->codec;
	t->width = hdr->width;
	t->height = hdr->height;
	t->fps_num = hdr->fps_num;
	t->read_seq = 0;
	t->params.ready = false;
	t->incarnation = atomic_load(&hdr->incarnation);
	t->idle = false;
	t->idle_since_us = rss_timestamp_us();
	t->idle_check_us = 0;
	rss_ring_acquire(t->ring);

	if (is_primary(t)) {
		/* Codec or geometry may have changed: a timelapse track
		 * cannot bend mid-file. The new ring also restarts its
		 * sequence space, so the sampler's claimed-seq guard must
		 * forget the old one or it rejects every new frame. */
		if (st->tl_mux)
			close_timelapse(st);
		rmr_tl_ring_reset(&st->tl);
	}

	if (t->attached_once) {
		RSS_DEBUG("video ring reconnected (%s)", t->ring_name);
	} else {
		const char *name = t->codec == 2 ? "MJPEG" : t->codec == 1 ? "H.265" : "H.264";
		if (is_primary(t))
			RSS_INFO("video: %s %ux%u @ %u fps", name, t->width, t->height, t->fps_num);
		else
			RSS_INFO("video %s (tag %s): %s %ux%u @ %u fps", t->ring_name, t->tag, name,
				 t->width, t->height, t->fps_num);
	}
	t->attached_once = true;

	uint32_t fps =
		hdr->fps_den > 1 ? (hdr->fps_num + hdr->fps_den - 1) / hdr->fps_den : hdr->fps_num;
	track_prebuf_init(st, t, fps);
	return 0;
}

static bool clip_wanted(rmr_state_t *st)
{
	if (st->mode == RMR_MODE_BOTH)
		return atomic_load(&st->clip_recording);
	if (st->mode == RMR_MODE_MOTION)
		return atomic_load(&st->recording);
	return false;
}

/* Stops apply whether or not frames flow. A silent or detached source
 * never reaches its next keyframe, so its files end at their period. */
static void track_housekeep(rmr_state_t *st, rmr_track_t *t, bool rec, bool clip_want)
{
	if (t->mux && !rec) {
		close_segment(t);
		if (is_primary(t))
			RSS_INFO("recording stopped");
	}
	if (t->clip_mux && !clip_want)
		close_clip(t);
	if (t->ring && !t->idle)
		return;
	if (t->mux && rss_wallclock_us() >= t->segment_boundary_rt_us) {
		close_segment(t);
		rmr_storage_enforce_limit(st->storage);
	}
	if (t->clip_mux && st->clip_length_sec > 0 &&
	    rss_timestamp_us() - t->clip_start_us >= (int64_t)st->clip_length_sec * 1000000)
		close_clip(t);
}

static void track_write_segment(rmr_track_t *t, const rss_ring_slot_t *meta,
				const uint8_t *mux_data, uint32_t mux_len)
{
	rmr_state_t *st = t->st;

	/* Flush fragment periodically for steady write pattern.
	 * On keyframe: also check segment rotation. */
	if (t->mux) {
		t->frames_since_flush++;
		/* Ask the encoder for an IDR on the first frame past the
		 * wall-clock boundary so the split lands within a frame or
		 * two of :00 instead of a GOP late. Requesting BEFORE the
		 * boundary is a trap: the forced key arrives before the
		 * boundary too, cannot rotate, and restarts the GOP, pushing
		 * the first eligible key a full GOP past :00 (constant +2s
		 * segments on 20fps HEVC). */
		int64_t now_rt = rss_wallclock_us();
		if (!t->segment_idr_requested && !meta->is_key &&
		    now_rt >= t->segment_boundary_rt_us) {
			rss_ring_request_idr(t->ring);
			t->segment_idr_requested = true;
		}
		if (meta->is_key) {
			rmr_mux_flush_fragment(t->mux);
			if (st->sign_enabled)
				rmr_sign_stream_emit(&t->sign_seg, false, direct_write, t);
			t->frames_since_flush = 0;
			if (rmr_storage_should_rotate_at(st->storage, t->segment_start_rt_us,
							 now_rt)) {
				close_segment(t);
				rmr_storage_enforce_limit(st->storage);
			}
		} else if (t->frames_since_flush >= 10) {
			rmr_mux_flush_fragment(t->mux);
			if (st->sign_enabled)
				rmr_sign_stream_emit(&t->sign_seg, false, direct_write, t);
			t->frames_since_flush = 0;
		}
	}

	/* Start new segment if needed */
	if (!t->mux) {
		if (!meta->is_key || start_segment(t) < 0)
			return;
		t->v_ts_base = -1;
		t->a_dts = 0;
		t->a_ts_base = -1;
		t->frames_written = 0;
		t->bytes_written = 0;
	}

	/* DTS from ring timestamp */
	if (t->v_ts_base < 0)
		t->v_ts_base = meta->timestamp;
	int64_t v_dts = (meta->timestamp - t->v_ts_base) * 90 / 1000;

	rmr_video_sample_t vs = {
		.data = mux_data,
		.size = mux_len,
		.dts = v_dts,
		.pts = v_dts,
		.is_key = meta->is_key,
	};

	if (rmr_mux_write_video(t->mux, &vs) == 0)
		t->frames_written++;
	else
		t->frames_dropped++;
}

static void track_write_clip(rmr_track_t *t, bool clip_want, const rss_ring_slot_t *meta,
			     const uint8_t *mux_data, uint32_t mux_len)
{
	rmr_state_t *st = t->st;

	/* Start clip with pre-buffer */
	if (clip_want && !t->clip_mux && rmr_storage_available(st->clip_storage)) {
		if (open_clip_with_prebuffer(t) == 0) {
			RSS_INFO("motion clip active");
			/* the seconds before the event are on the medium first */
			rmr_io_sync(st->io, t->clip_fd);
		}
	}
	if (!t->clip_mux)
		return;

	clip_write_video(t, mux_data, mux_len, meta->timestamp, meta->is_key);

	/* Check clip length cap */
	if (st->clip_length_sec > 0) {
		int64_t elapsed = rss_timestamp_us() - t->clip_start_us;
		if (elapsed >= (int64_t)st->clip_length_sec * 1000000) {
			close_clip(t);
			/* If still triggered, open a continuation clip (no
			 * pre-buffer on continuation) */
			if (clip_want && open_clip(t) == 0)
				RSS_INFO("clip rotated (length cap)");
		}
	}
}

/* Prepare one frame read from the track's ring and feed every file. */
static void track_frame(rmr_state_t *st, rmr_track_t *t, uint32_t length,
			const rss_ring_slot_t *meta)
{
	bool rec = atomic_load(&st->recording);

	/* The encoder's frame counter exposes every frame that never
	 * reached rmr (ring overflow, producer drops) */
	if (meta->src_seq != RSS_SRC_SEQ_NONE) {
		uint32_t gap = meta->src_seq - t->last_src_seq - 1;
		if (t->last_src_seq != RSS_SRC_SEQ_NONE && gap > 0 && gap < RSS_SRC_SEQ_MAX_GAP &&
		    (t->mux || t->clip_mux || rec)) {
			t->frames_lost += gap;
			RSS_WARN("%u source frame%s lost before recording (%s, %" PRIu64 " total)",
				 gap, gap == 1 ? "" : "s", t->ring_name, t->frames_lost);
		}
		t->last_src_seq = meta->src_seq;
	}

	if (t->wait_key) {
		if (!meta->is_key) {
			if (t->mux || t->clip_mux)
				t->frames_dropped++;
			return;
		}
		t->wait_key = false;
	}

	/* Prepare frame data for muxer */
	const uint8_t *mux_data;
	uint32_t mux_len;
	bool is_mjpeg = (t->codec == 2);

	if (is_mjpeg) {
		if (!t->params.ready)
			t->params.ready = true;
		mux_data = t->frame_buf;
		mux_len = length;
	} else {
		if (meta->is_key && !t->params.ready)
			rmr_extract_params(t->frame_buf, length, t->codec, &t->params);
		if (!t->params.ready)
			return;

		/* ST 0604 timecode SEI leads the sample; capture UTC comes
		 * from the producer's ring clock mapping. */
		uint32_t sei_len = 0;
		int64_t utc_off;
		uint8_t utc_st;
		if (st->sei_timecode && rss_ring_get_utc(t->ring, &utc_off, &utc_st) == 0) {
			int n = rss_sei_build_timestamp(
				t->avcc_buf, t->avcc_buf_size, (int)t->codec, RSS_SEI_PREFIX_AVCC,
				(uint64_t)(meta->timestamp + utc_off), utc_st);
			if (n > 0)
				sei_len = (uint32_t)n;
		}

		int avcc_len = rmr_annexb_to_avcc(t->frame_buf, length, t->avcc_buf + sei_len,
						  t->avcc_buf_size - sei_len, t->codec);
		if (avcc_len <= 0)
			return;

		if (avcc_len >= 5) {
			const uint8_t *fb = t->avcc_buf + sei_len;
			uint32_t nal_len = ((uint32_t)fb[0] << 24) | ((uint32_t)fb[1] << 16) |
					   ((uint32_t)fb[2] << 8) | (uint32_t)fb[3];
			if (nal_len > (uint32_t)avcc_len - 4) {
				RSS_WARN("AVCC corrupt: nal_len=%u avcc_len=%d", nal_len, avcc_len);
				return;
			}
		}
		mux_data = t->avcc_buf;
		mux_len = sei_len + (uint32_t)avcc_len;
	}

	/* Push to video pre-buffer (always, for motion clips) */
	if (t->video_pb) {
		track_prebuf_measure(st, t, mux_len, meta->timestamp);
		rmr_prebuf_push(t->video_pb, mux_data, mux_len, meta->timestamp, meta->is_key);
	}

	/* Continuous destination absent (standby covers the
	 * nothing-writable case, so clips may still be live): skip without
	 * stalling — sleeping here starves the ring reader into overflow,
	 * and every overflow requests an IDR. */
	if (rec && rmr_storage_available(st->storage))
		track_write_segment(t, meta, mux_data, mux_len);

	if (st->mode == RMR_MODE_MOTION || st->mode == RMR_MODE_BOTH)
		track_write_clip(t, clip_wanted(st), meta, mux_data, mux_len);

	/* ── Timelapse sampling (first track) ── */
	if (is_primary(t) && st->tl_enabled && (t->params.ready || is_mjpeg)) {
		bool was_armed = st->tl.want_sample;
		rmr_tl_tick(&st->tl, rss_timestamp_us());
		/* The sample must be a keyframe; ask for one the moment a
		 * tick arms so the wait is one frame, not the rest of a GOP.
		 * Requests coalesce with the segment splitter's own. */
		if (!was_armed && st->tl.want_sample && !is_mjpeg)
			rss_ring_request_idr(t->ring);

		bool tl_key = meta->is_key || is_mjpeg;
		if (tl_key && rmr_tl_take(&st->tl, t->read_seq))
			timelapse_write_sample(st, mux_data, mux_len);
	}
}

/* No frame for RING_IDLE_US: a ring the producer replaced (new inode,
 * or recreated in place under a new incarnation) is reopened; a ring
 * that is merely silent (rear camera unplugged) stays open and its
 * files close at their period's end (track_housekeep). */
static void track_check_idle(rmr_track_t *t, int64_t now)
{
	if (now - t->idle_since_us < RING_IDLE_US || now < t->idle_check_us)
		return;
	t->idle_check_us = now + RING_IDLE_US;
	if (rss_ring_stale(t->ring) ||
	    atomic_load(&rss_ring_get_header(t->ring)->incarnation) != t->incarnation) {
		RSS_DEBUG("video ring replaced, reopening (%s)", t->ring_name);
		track_detach(t);
		t->retry_us = 0;
		return;
	}
	if (!t->idle) {
		t->idle = true;
		RSS_DEBUG("video ring idle (%s)", t->ring_name);
	}
}

/* Read and record what the track's ring holds, at most a burst, so no
 * track waits on another. Returns the frames consumed. A missing or
 * idle ring is retried lazily without blocking the loop. */
static int track_service(rmr_state_t *st, rmr_track_t *t)
{
	int64_t now = rss_timestamp_us();
	if (!t->ring) {
		if (now < t->retry_us)
			return 0;
		if (track_attach(st, t) < 0) {
			t->retry_us = now + RING_RETRY_US;
			return 0;
		}
	}

	int n = 0;
	while (n < TRACK_READ_BURST) {
		uint32_t length;
		rss_ring_slot_t meta;
		int ret = rss_ring_read(t->ring, &t->read_seq, t->frame_buf, t->frame_buf_size,
					&length, &meta);
		if (ret == RSS_EOVERFLOW) {
			/* Frames were lost: anything until the next keyframe
			 * references pictures we never read, and writing it
			 * puts undecodable GOP fragments in the recording. */
			rss_ring_request_idr(t->ring);
			if (t->mux || t->clip_mux)
				t->frames_dropped++;
			t->wait_key = true;
		}
		if (ret == RSS_EOVERFLOW || ret == -EAGAIN) {
			track_check_idle(t, now);
			break;
		}
		n++;
		if (ret != 0)
			continue;
		t->idle_since_us = now;
		if (t->idle) {
			/* the decoder state across the silence is unknown */
			t->idle = false;
			if (!meta.is_key) {
				t->wait_key = true;
				rss_ring_request_idr(t->ring);
			}
			RSS_DEBUG("video ring active (%s)", t->ring_name);
		}
		track_frame(st, t, length, &meta);
	}
	return n;
}

static void audio_ring_params(rmr_state_t *st)
{
	const rss_ring_header_t *ahdr = rss_ring_get_header(st->audio_ring);
	st->audio_codec = ahdr->codec;
	st->audio_sample_rate = ahdr->fps_num;
	st->audio_aot = ahdr->profile;
	st->audio_bps =
		(st->audio_codec == RMR_AUDIO_PCMU || st->audio_codec == RMR_AUDIO_PCMA) ? 1 : 2;
	st->audio_samples_per_frame = 0;
	/* HE-AAC frames carry 2048 samples; the producer declares the size
	 * in the ring header. */
	if (st->audio_codec == RMR_AUDIO_AAC)
		st->audio_samples_per_frame = ahdr->width ? ahdr->width : 1024;
	else if (st->audio_codec == RMR_AUDIO_OPUS)
		st->audio_samples_per_frame = st->audio_sample_rate / 50;
}

static void audio_write_tracks(rmr_state_t *st, const uint8_t *data, uint32_t len, uint32_t samples,
			       int64_t ts)
{
	for (int i = 0; i < st->track_count; i++) {
		rmr_track_t *t = &st->tracks[i];
		if (t->mux) {
			t->a_dts = steer_audio_dts(t->a_dts, ts, &t->a_ts_base, samples,
						   st->audio_sample_rate);
			rmr_audio_sample_t as = {
				.data = data,
				.size = len,
				.dts = t->a_dts,
			};
			rmr_mux_write_audio(t->mux, &as);
			t->a_dts += samples;
		}
		clip_write_audio(t, data, len, samples, ts);
	}
}

/* One audio ring feeds every track's files. Returns frames consumed. */
static int audio_service(rmr_state_t *st)
{
	int64_t now = rss_timestamp_us();
	if (!st->audio_ring) {
		/* Lazy attach, retried every ~2s until found */
		if (!st->audio_enabled || now < st->audio_retry_us)
			return 0;
		st->audio_retry_us = now + AUDIO_RETRY_US;
		st->audio_ring = rss_ring_open("audio");
		if (!st->audio_ring)
			return 0;
		rss_ring_check_version(st->audio_ring, "audio");
		audio_ring_params(st);
		/* Start reading from current position (skip stale frames) */
		st->audio_read_seq = atomic_load(&rss_ring_get_header(st->audio_ring)->write_seq);
		st->audio_last_ws = 0;
		st->audio_idle_since_us = now;
		RSS_DEBUG("audio ring attached (late): codec=%u rate=%u frame_samples=%u aot=%u",
			  st->audio_codec, st->audio_sample_rate, st->audio_samples_per_frame,
			  st->audio_aot);
	}

	int n = 0;
	while (n < AUDIO_READ_BURST) {
		uint32_t alen;
		rss_ring_slot_t ameta;
		int ret = rss_ring_read(st->audio_ring, &st->audio_read_seq, st->audio_buf,
					sizeof(st->audio_buf), &alen, &ameta);
		if (ret != 0)
			break;
		n++;
		if (st->audio_pb)
			rmr_prebuf_push(st->audio_pb, st->audio_buf, alen, ameta.timestamp, 0);
		audio_write_tracks(st, st->audio_buf, alen, audio_frame_samples(st, alen),
				   ameta.timestamp);
	}

	/* Idle detection — close audio ring if RAD stopped */
	uint64_t aws = rss_ring_get_header(st->audio_ring)->write_seq;
	if (aws != st->audio_last_ws) {
		st->audio_last_ws = aws;
		st->audio_idle_since_us = now;
	} else if (n == 0 && now - st->audio_idle_since_us >= RING_IDLE_US) {
		RSS_DEBUG("audio ring idle, closing");
		rss_ring_close(st->audio_ring);
		st->audio_ring = NULL;
		st->audio_retry_us = now + AUDIO_RETRY_US;
	}
	return n;
}

static void close_all_files(rmr_state_t *st)
{
	for (int i = 0; i < st->track_count; i++) {
		if (st->tracks[i].mux)
			close_segment(&st->tracks[i]);
		if (st->tracks[i].clip_mux)
			close_clip(&st->tracks[i]);
	}
}

static void record_loop(rmr_state_t *st)
{
	int ctrl_fd = st->ctrl ? rss_ctrl_get_fd(st->ctrl) : -1;
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
				close_all_files(st);
				for (int i = 0; i < st->track_count; i++)
					track_detach(&st->tracks[i]);
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

		telematics_poll(st);
		io_service(st);

		bool rec = atomic_load(&st->recording);
		bool clip_want = clip_wanted(st);
		int busy = 0;
		for (int i = 0; i < st->track_count; i++) {
			track_housekeep(st, &st->tracks[i], rec, clip_want);
			busy += track_service(st, &st->tracks[i]);
		}
		busy += audio_service(st);
		if (busy)
			continue;

		rss_ring_t *wait_ring = NULL;
		for (int i = 0; i < st->track_count && !wait_ring; i++)
			wait_ring = st->tracks[i].ring;
		if (wait_ring)
			rss_ring_wait(wait_ring, LOOP_WAIT_MS);
		else
			usleep(LOOP_WAIT_MS * 1000);
	}

	close_all_files(st);
}

/* ── Entry point ── */

static void tracks_init(rmr_state_t *st)
{
	rmr_stream_spec_t specs[RMR_MAX_TRACKS];
	st->track_count = rmr_streams_parse(
		rss_config_get_str(st->cfg, "recording", "streams", NULL),
		rss_config_get_int(st->cfg, "recording", "stream", 0), specs, RMR_MAX_TRACKS);
	for (int i = 0; i < st->track_count; i++) {
		rmr_track_t *t = &st->tracks[i];
		t->st = st;
		rss_strlcpy(t->ring_name, specs[i].ring, sizeof(t->ring_name));
		rss_strlcpy(t->tag, specs[i].tag, sizeof(t->tag));
		t->segment_fd = -1;
		t->clip_fd = -1;
		t->v_ts_base = -1;
		t->a_ts_base = -1;
		t->last_src_seq = RSS_SRC_SEQ_NONE;
	}
}

static void tracks_destroy(rmr_state_t *st)
{
	for (int i = 0; i < st->track_count; i++) {
		rmr_track_t *t = &st->tracks[i];
		track_detach(t);
		if (t->video_pb)
			rmr_prebuf_destroy(t->video_pb);
		free(t->frame_buf);
		free(t->avcc_buf);
	}
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
	st.tl_fd = -1;
	st.tl_enabled = tl_enabled;
	rmr_tl_init(&st.tl, rss_config_get_int(dctx.cfg, "timelapse", "interval", 10),
		    rss_config_get_int(dctx.cfg, "timelapse", "playback_fps", 30),
		    rss_config_get_int(dctx.cfg, "timelapse", "file_frames", 0));
	st.audio_enabled = rss_config_get_bool(dctx.cfg, "recording", "audio", true);
	st.sei_timecode = rss_config_get_bool(dctx.cfg, "recording", "sei_timecode", true);
	st.sync_interval_ms = rss_config_get_int(dctx.cfg, "recording", "sync_interval_ms", 1000);
	tracks_init(&st);
	int buffer_kb = rss_config_get_int(dctx.cfg, "recording", "write_buffer_kb", 4096);
	st.io = rmr_io_create((uint32_t)(buffer_kb > 0 ? buffer_kb : 4096) * 1024);
	if (!st.io) {
		RSS_FATAL("cannot start the writer thread");
		goto cleanup;
	}
	st.gps = rss_config_get_bool(dctx.cfg, "recording", "gps", true);
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
	int prebuf_mb = rss_config_get_int(dctx.cfg, "recording", "prebuffer_max_mb", 16);
	if (prebuf_mb < 1)
		prebuf_mb = 1;
	if (prebuf_mb > 64)
		prebuf_mb = 64;
	st.prebuffer_max_bytes = (uint32_t)prebuf_mb * 1024 * 1024;

	st.clip_length_sec = rss_config_get_int(dctx.cfg, "recording", "clip_length_sec", 60);

	/* The first stream is required at startup; the others attach when
	 * their producers appear. */
	rmr_track_t *t0 = &st.tracks[0];
	for (int attempt = 0; attempt < 30 && *st.running; attempt++) {
		if (track_attach(&st, t0) == 0)
			break;
		RSS_DEBUG("waiting for %s ring...", t0->ring_name);
		sleep(1);
	}
	if (!t0->ring) {
		RSS_FATAL("video ring not available");
		goto cleanup;
	}
	for (int i = 1; i < st.track_count; i++)
		if (track_attach(&st, &st.tracks[i]) < 0)
			RSS_INFO("video %s not available yet (tag %s)", st.tracks[i].ring_name,
				 st.tracks[i].tag);

	/* Open audio ring */
	if (st.audio_enabled) {
		st.audio_ring = rss_ring_open("audio");
		if (st.audio_ring) {
			rss_ring_check_version(st.audio_ring, "audio");
			audio_ring_params(&st);
			st.audio_idle_since_us = rss_timestamp_us();
			RSS_DEBUG("audio: codec=%u rate=%u aot=%u", st.audio_codec,
				  st.audio_sample_rate, st.audio_aot);
		} else {
			RSS_WARN("audio ring not available (recording video only)");
			st.audio_retry_us = rss_timestamp_us() + AUDIO_RETRY_US;
		}
	}

	/* Audio pre-buffer for motion clips, shared by every track */
	if (st.audio_ring && st.prebuffer_sec > 0 &&
	    (st.mode == RMR_MODE_MOTION || st.mode == RMR_MODE_BOTH)) {
		/* Audio: 50fps (20ms frames) typical */
		uint32_t a_frames = 50 * (uint32_t)st.prebuffer_sec + 50;
		uint32_t a_slots = next_pow2(a_frames);
		/* Audio data: small frames, 1KB each is generous */
		uint32_t a_data = a_slots * 1024;
		if (a_data < 128 * 1024)
			a_data = 128 * 1024;

		st.audio_pb = rmr_prebuf_create(a_slots, a_data);
		if (st.audio_pb)
			RSS_DEBUG("audio pre-buffer: %u slots, %u KB data", a_slots, a_data / 1024);
		else
			RSS_WARN("audio pre-buffer alloc failed");
	}

	/* Storage */
	rmr_storage_config_t scfg = {
		.base_path = rss_config_get_str(dctx.cfg, "recording", "storage_path", ""),
		.segment_minutes = rss_config_get_int(dctx.cfg, "recording", "segment_minutes", 5),
		.segment_seconds = rss_config_get_int(dctx.cfg, "recording", "segment_seconds", 0),
		.max_storage_mb = rss_config_get_int(dctx.cfg, "recording", "max_storage_mb", 0),
		.min_free_mb = rss_config_get_int(dctx.cfg, "recording", "min_free_mb", 0),
	};
	st.storage = rmr_storage_create(&scfg);
	if (!st.storage) {
		RSS_FATAL("storage init failed");
		goto cleanup;
	}
	RSS_INFO("storage: %s%s", rmr_storage_path(st.storage), scfg.base_path[0] ? "" : " (auto)");
	const char *root_cfg = scfg.base_path;

	/* Control socket */
	rss_mkdir_p(RSS_RUN_DIR);
	st.ctrl = rss_ctrl_listen(RSS_RUN_DIR "/rmr.sock");
	if (!st.ctrl)
		RSS_WARN("control socket failed (non-fatal)");

	/* Set up clip storage for motion modes */
	if (st.mode == RMR_MODE_BOTH || st.mode == RMR_MODE_MOTION) {
		char clip_path[280] = "";
		if (root_cfg[0])
			snprintf(clip_path, sizeof(clip_path), "%s/clips", root_cfg);
		rmr_storage_config_t ccfg = {
			.base_path = clip_path,
			.subdir = "/clips",
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
		char tl_path[280] = "";
		if (root_cfg[0])
			snprintf(tl_path, sizeof(tl_path), "%s/timelapse", root_cfg);
		rmr_storage_config_t tcfg = {
			.base_path = tl_path,
			.subdir = "/timelapse",
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
	close_all_files(&st);
	if (st.tl_mux)
		close_timelapse(&st);
	if (st.tl_storage)
		rmr_storage_destroy(st.tl_storage);
	if (st.clip_storage)
		rmr_storage_destroy(st.clip_storage);
	if (st.storage)
		rmr_storage_destroy(st.storage);
	tracks_destroy(&st);
	if (st.audio_ring)
		rss_ring_close(st.audio_ring);
	if (st.tele_ring)
		rss_ring_close(st.tele_ring);
	if (st.audio_pb)
		rmr_prebuf_destroy(st.audio_pb);
	rmr_io_destroy(st.io); /* drains: every queued byte and close runs */
	rss_config_free(dctx.cfg);

	rss_daemon_cleanup("rmr");

	return 0;
}

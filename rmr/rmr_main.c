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
			}
			return -1;
		}
	}
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
			return -1;
		}
	}
	return 0;
}

/* ── Segment management ── */

static void setup_mux_tracks(rmr_mux_t *mux, rmr_state_t *st)
{
	rmr_video_params_t vp = {
		.codec = (st->video_codec == 1) ? RMR_CODEC_H265 : RMR_CODEC_H264,
		.width = st->width,
		.height = st->height,
		.timescale = 90000,
	};
	rmr_mux_set_video(mux, &vp, st->params.sps, st->params.sps_len, st->params.pps,
			  st->params.pps_len, st->params.vps_len > 0 ? st->params.vps : NULL,
			  st->params.vps_len);

	if (st->audio_ring) {
		rmr_audio_params_t ap = {.sample_rate = st->audio_sample_rate, .channels = 1};
		switch (st->audio_codec) {
		case RMR_AUDIO_PCMU: ap.codec = RMR_AUDIO_PCMU; ap.bits_per_sample = 8; break;
		case RMR_AUDIO_PCMA: ap.codec = RMR_AUDIO_PCMA; ap.bits_per_sample = 8; break;
		case RMR_AUDIO_AAC:  ap.codec = RMR_AUDIO_AAC;  ap.bits_per_sample = 16; break;
		case RMR_AUDIO_OPUS: ap.codec = RMR_AUDIO_OPUS; ap.bits_per_sample = 16; break;
		default:             ap.codec = RMR_AUDIO_L16;   ap.bits_per_sample = 16; break;
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
	rmr_mux_start(st->mux);
	st->segment_start_us = rss_timestamp_us();

	RSS_INFO("recording segment: %s", st->segment_path);
	return 0;
}

static void close_segment(rmr_state_t *st)
{
	if (st->mux) {
		rmr_mux_finalize(st->mux);
		rmr_mux_destroy(st->mux);
		st->mux = NULL;
	}

	int fd = st->segment_fd;
	st->segment_fd = -1;

	if (fd >= 0) {
		rmr_storage_close_segment(fd);
		RSS_INFO("segment closed: %s (%" PRIu64 " frames, %" PRIu64 " bytes)",
			 st->segment_path, st->frames_written, st->bytes_written);
	}
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
	st->clip_start_us = rss_timestamp_us();
	st->clip_bytes = 0;

	setup_mux_tracks(st->clip_mux, st);
	rmr_mux_start(st->clip_mux);

	RSS_INFO("motion clip started: %s", st->clip_path);
	return 0;
}

static void close_clip(rmr_state_t *st)
{
	if (st->clip_mux) {
		rmr_mux_finalize(st->clip_mux);
		rmr_mux_destroy(st->clip_mux);
		st->clip_mux = NULL;
	}
	if (st->clip_fd >= 0) {
		rmr_storage_close_segment(st->clip_fd);
		RSS_INFO("motion clip closed: %s (%" PRIu64 " bytes)", st->clip_path, st->clip_bytes);
		st->clip_fd = -1;
	}
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

	if (is_key)
		rmr_mux_flush_fragment(st->clip_mux);
	rmr_mux_write_video(st->clip_mux, &vs);
}

/* Write an audio frame to the clip mux with independent DTS. */
static void clip_write_audio(rmr_state_t *st, const uint8_t *data, uint32_t len,
			     uint32_t samples)
{
	if (!st->clip_mux)
		return;

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
	int count;      /* frames replayed so far */
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
	uint32_t samples = rc->audio_samples_per_frame
				   ? rc->audio_samples_per_frame
				   : slot->data_length / rc->audio_bps;
	clip_write_audio(rc->st, data, slot->data_length, samples);
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
	uint32_t vsi = vstart & st->video_pb->mask;
	int64_t kf_ts = st->video_pb->slots[vsi].timestamp;
	uint32_t newest_vi = (st->video_pb->write_idx - 1) & st->video_pb->mask;
	int64_t newest_v_ts = st->video_pb->slots[newest_vi].timestamp;
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
	RSS_INFO("pre-buffer: replayed %d video frames (%.1fs)",
		 vcount, v_duration_us / 1000000.0);

	/* Replay audio pre-buffer — match video duration by frame count.
	 * Timestamp matching is unreliable across rings, so calculate
	 * how many audio frames fit in the video pre-buffer duration. */
	if (st->audio_pb && st->audio_pb->count > 0 && v_duration_us > 0) {
		/* Audio frame duration in microseconds */
		int64_t audio_frame_us;
		if (audio_samples_per_frame > 0 && st->audio_sample_rate > 0)
			audio_frame_us = (int64_t)audio_samples_per_frame * 1000000
					 / st->audio_sample_rate;
		else
			audio_frame_us = 20000; /* fallback: 20ms */

		int audio_frame_count = (int)(v_duration_us / audio_frame_us);
		if (audio_frame_count < 1)
			audio_frame_count = 1;
		if ((uint32_t)audio_frame_count > st->audio_pb->count)
			audio_frame_count = (int)st->audio_pb->count;

		/* Rewind from the head by audio_frame_count frames */
		uint32_t astart = st->audio_pb->write_idx - (uint32_t)audio_frame_count;

		rc.max_frames = audio_frame_count;
		rc.count = 0;
		int acount = rmr_prebuf_iterate(st->audio_pb, astart,
						replay_audio_frame, &rc);
		RSS_INFO("pre-buffer: replayed %d audio frames (target %d, %.1fs)",
			 acount, audio_frame_count, v_duration_us / 1000000.0);
	}

	return 0;
}

/* ── Control socket handler ── */

static int rmr_ctrl_handler(const char *cmd_json, char *resp_buf, int resp_buf_size, void *userdata)
{
	rmr_state_t *st = userdata;

	if (strstr(cmd_json, "\"start\"")) {
		if (st->mode == RMR_MODE_MOTION)
			atomic_store(&st->recording, true);
		if (st->mode == RMR_MODE_BOTH)
			atomic_store(&st->clip_recording, true);
		snprintf(resp_buf, resp_buf_size, "{\"status\":\"ok\"}");
		return (int)strlen(resp_buf);
	}

	if (strstr(cmd_json, "\"stop\"")) {
		if (st->mode == RMR_MODE_MOTION)
			atomic_store(&st->recording, false);
		if (st->mode == RMR_MODE_BOTH)
			atomic_store(&st->clip_recording, false);
		snprintf(resp_buf, resp_buf_size, "{\"status\":\"ok\"}");
		return (int)strlen(resp_buf);
	}

	if (strstr(cmd_json, "\"status\"")) {
		snprintf(resp_buf, resp_buf_size,
			 "{\"recording\":%s,\"clip\":%s,\"mode\":%d,"
			 "\"file\":\"%s\",\"frames\":%" PRIu64
			 ",\"dropped\":%" PRIu64 ",\"bytes\":%" PRIu64 "}",
			 atomic_load(&st->recording) ? "true" : "false",
			 atomic_load(&st->clip_recording) ? "true" : "false",
			 st->mode, st->segment_path,
			 st->frames_written, st->frames_dropped, st->bytes_written);
		return (int)strlen(resp_buf);
	}

	snprintf(resp_buf, resp_buf_size, "{\"status\":\"ok\"}");
	return (int)strlen(resp_buf);
}

/* ── Main loop ── */

static void record_loop(rmr_state_t *st)
{
	int64_t v_ts_base = -1; /* continuous segment video timestamp base */

	/* Audio DTS increment per ring frame. */
	uint32_t audio_bps = (st->audio_codec == RMR_AUDIO_PCMU ||
			      st->audio_codec == RMR_AUDIO_PCMA) ? 1 : 2;
	uint32_t audio_samples_per_frame = 0;
	if (st->audio_codec == RMR_AUDIO_AAC)
		audio_samples_per_frame = 1024;
	else if (st->audio_codec == RMR_AUDIO_OPUS)
		audio_samples_per_frame = st->audio_sample_rate / 50;
	int64_t a_dts_counter = 0;
	bool was_recording = false;

	int ctrl_fd = st->ctrl ? rss_ctrl_get_fd(st->ctrl) : -1;

	while (*st->running) {
		/* Handle control socket */
		if (ctrl_fd >= 0) {
			fd_set fds;
			struct timeval tv = {0, 0};
			FD_ZERO(&fds);
			FD_SET(ctrl_fd, &fds);
			if (select(ctrl_fd + 1, &fds, NULL, NULL, &tv) > 0)
				rss_ctrl_accept_and_handle(st->ctrl, rmr_ctrl_handler, st);
		}

		/* ── Read video frame from ring ── */
		uint32_t length;
		rss_ring_slot_t meta;
		int ret = rss_ring_read(st->video_ring, &st->video_read_seq, st->frame_buf,
					st->frame_buf_size, &length, &meta);

		if (ret == RSS_EOVERFLOW) {
			rss_ring_request_idr(st->video_ring);
			st->frames_dropped++;
			continue;
		}
		if (ret == -EAGAIN) {
			rss_ring_wait(st->video_ring, 40);
			continue;
		}
		if (ret != 0)
			continue;

		/* Extract codec params from first keyframe */
		if (meta.is_key && !st->params.ready)
			rmr_extract_params(st->frame_buf, length, st->video_codec, &st->params);
		if (!st->params.ready)
			continue;

		/* Convert Annex B to AVCC */
		int avcc_len = rmr_annexb_to_avcc(st->frame_buf, length, st->avcc_buf,
						  st->avcc_buf_size, st->video_codec);
		if (avcc_len <= 0)
			continue;

		/* Verify AVCC: first 4 bytes must be a valid NAL length */
		if (avcc_len >= 5) {
			uint32_t nal_len = ((uint32_t)st->avcc_buf[0] << 24) |
					   ((uint32_t)st->avcc_buf[1] << 16) |
					   ((uint32_t)st->avcc_buf[2] << 8) |
					   (uint32_t)st->avcc_buf[3];
			if (nal_len > (uint32_t)avcc_len - 4) {
				RSS_WARN("AVCC corrupt: nal_len=%u avcc_len=%d", nal_len, avcc_len);
				continue;
			}
		}

		/* Push to video pre-buffer (always, for motion clips) */
		if (st->video_pb)
			rmr_prebuf_push(st->video_pb, st->avcc_buf, (uint32_t)avcc_len,
					meta.timestamp, meta.is_key);

		/* ── Read audio frames and push to pre-buffer ── */
		/* Audio frame data saved for writing below */
		struct {
			uint8_t data[8192];
			uint32_t len;
			uint32_t samples;
		} audio_frames[4];
		int audio_count = 0;

		if (st->audio_ring) {
			for (int burst = 0; burst < 4; burst++) {
				uint32_t alen;
				rss_ring_slot_t ameta;
				ret = rss_ring_read(st->audio_ring, &st->audio_read_seq,
						    st->audio_buf, sizeof(st->audio_buf),
						    &alen, &ameta);
				if (ret == RSS_EOVERFLOW || ret != 0)
					break;

				/* Push to audio pre-buffer */
				if (st->audio_pb)
					rmr_prebuf_push(st->audio_pb, st->audio_buf, alen,
							ameta.timestamp, 0);

				/* Save for writing to muxers */
				if (audio_count < 4) {
					memcpy(audio_frames[audio_count].data, st->audio_buf, alen);
					audio_frames[audio_count].len = alen;
					audio_frames[audio_count].samples =
						audio_samples_per_frame
							? audio_samples_per_frame
							: alen / audio_bps;
					audio_count++;
				}
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
			RSS_INFO("recording stopped");
		}

		if (rec) {
			/* Wait for storage */
			if (!rmr_storage_available(st->storage)) {
				usleep(1000000);
				goto clip_handling;
			}

			/* On keyframe: flush previous fragment, check rotation */
			if (meta.is_key && st->mux) {
				rmr_mux_flush_fragment(st->mux);
				if (rmr_storage_should_rotate(st->storage, st->segment_start_us)) {
					close_segment(st);
					rmr_storage_enforce_limit(st->storage);
				}
			}

			/* Start new segment if needed */
			if (!st->mux) {
				if (meta.is_key) {
					if (start_segment(st) < 0)
						goto clip_handling;
					v_ts_base = -1;
					a_dts_counter = 0;
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
				.data = st->avcc_buf,
				.size = (uint32_t)avcc_len,
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
			if (clip_want && !st->clip_mux) {
				if (open_clip_with_prebuffer(st, audio_samples_per_frame,
							    audio_bps) == 0) {
		RSS_INFO("motion clip active");
				}
			}

			/* Write live frames to clip */
			if (st->clip_mux) {
				clip_write_video(st, st->avcc_buf, (uint32_t)avcc_len,
						 meta.timestamp, meta.is_key);

				for (int i = 0; i < audio_count; i++)
					clip_write_audio(st, audio_frames[i].data,
							 audio_frames[i].len,
							 audio_frames[i].samples);

				/* Check clip length cap */
				if (st->clip_length_sec > 0) {
					int64_t elapsed = rss_timestamp_us() - st->clip_start_us;
					if (elapsed >= (int64_t)st->clip_length_sec * 1000000) {
						close_clip(st);
						/* If still triggered, open a continuation
						 * clip (no pre-buffer on continuation) */
						if (clip_want) {
							if (open_clip(st) == 0) {
								RSS_INFO("clip rotated (length cap)");
							}
						}
					}
				}
			}

			/* Stop clip */
			if (!clip_want && st->clip_mux)
				close_clip(st);
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
	int ret = rss_daemon_init(&dctx, "rmr", argc, argv);
	if (ret != 0)
		return ret < 0 ? 1 : 0;

	if (!rss_config_get_bool(dctx.cfg, "recording", "enabled", false)) {
		RSS_INFO("recording disabled in config");
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
	st.stream_idx = rss_config_get_int(dctx.cfg, "recording", "stream", 0);
	st.audio_enabled = rss_config_get_bool(dctx.cfg, "recording", "audio", true);

	/* Parse recording mode */
	const char *mode_str = rss_config_get_str(dctx.cfg, "recording", "mode", "continuous");
	if (strcmp(mode_str, "motion") == 0)
		st.mode = RMR_MODE_MOTION;
	else if (strcmp(mode_str, "both") == 0)
		st.mode = RMR_MODE_BOTH;
	else
		st.mode = RMR_MODE_CONTINUOUS;

	st.prebuffer_sec = rss_config_get_int(dctx.cfg, "recording", "prebuffer_sec", 5);
	if (st.prebuffer_sec < 0) st.prebuffer_sec = 0;
	if (st.prebuffer_sec > 5) st.prebuffer_sec = 5;

	st.clip_length_sec = rss_config_get_int(dctx.cfg, "recording", "clip_length_sec", 60);

	/* Open video ring */
	const char *ring_names[] = {"main", "sub"};
	const char *ring_name = ring_names[st.stream_idx < 2 ? st.stream_idx : 0];

	for (int attempt = 0; attempt < 30 && *st.running; attempt++) {
		st.video_ring = rss_ring_open(ring_name);
		if (st.video_ring)
			break;
		RSS_DEBUG("waiting for %s ring...", ring_name);
		sleep(1);
	}

	if (!st.video_ring) {
		RSS_FATAL("video ring not available");
		goto cleanup;
	}

	/* Read ring metadata */
	const rss_ring_header_t *vhdr = rss_ring_get_header(st.video_ring);
	st.video_codec = vhdr->codec;
	st.width = vhdr->width;
	st.height = vhdr->height;
	st.fps_num = vhdr->fps_num;
	RSS_INFO("video: %s %ux%u @ %u fps", st.video_codec == 1 ? "H.265" : "H.264", st.width,
		 st.height, st.fps_num);

	/* Allocate buffers */
	st.frame_buf_size = vhdr->data_size;
	st.frame_buf = malloc(st.frame_buf_size);
	st.avcc_buf_size = st.frame_buf_size;
	st.avcc_buf = malloc(st.avcc_buf_size);
	if (!st.frame_buf || !st.avcc_buf) {
		RSS_FATAL("buffer allocation failed (%u bytes)", st.frame_buf_size);
		goto cleanup;
	}

	/* Open audio ring */
	if (st.audio_enabled) {
		st.audio_ring = rss_ring_open("audio");
		if (st.audio_ring) {
			const rss_ring_header_t *ahdr = rss_ring_get_header(st.audio_ring);
			st.audio_codec = ahdr->codec;
			st.audio_sample_rate = ahdr->fps_num;
			RSS_INFO("audio: codec=%u rate=%u", st.audio_codec, st.audio_sample_rate);
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
		if (v_data < 1024 * 1024) v_data = 1024 * 1024;  /* min 1MB */
		if (v_data > 10 * 1024 * 1024) v_data = 10 * 1024 * 1024; /* max 10MB */

		st.video_pb = rmr_prebuf_create(v_slots, (uint32_t)v_data);
		if (st.video_pb)
			RSS_INFO("video pre-buffer: %u slots, %u KB data", v_slots,
				 (uint32_t)(v_data / 1024));
		else
			RSS_WARN("video pre-buffer alloc failed");

		if (st.audio_ring) {
			/* Audio: 50fps (20ms frames) typical */
			uint32_t a_frames = 50 * (uint32_t)st.prebuffer_sec + 50;
			uint32_t a_slots = next_pow2(a_frames);
			/* Audio data: small frames, 1KB each is generous */
			uint32_t a_data = a_slots * 1024;
			if (a_data < 128 * 1024) a_data = 128 * 1024;

			st.audio_pb = rmr_prebuf_create(a_slots, a_data);
			if (st.audio_pb)
				RSS_INFO("audio pre-buffer: %u slots, %u KB data", a_slots,
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
		.max_storage_mb = rss_config_get_int(dctx.cfg, "recording", "max_storage_mb", 0),
	};
	st.storage = rmr_storage_create(&scfg);
	if (!st.storage) {
		RSS_FATAL("storage init failed");
		goto cleanup;
	}

	/* Control socket */
	rss_mkdir_p("/var/run/rss");
	st.ctrl = rss_ctrl_listen("/var/run/rss/rmr.sock");
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
			.max_storage_mb = rss_config_get_int(dctx.cfg, "recording", "clip_max_mb", 100),
		};
		if (ccfg.segment_minutes < 1)
			ccfg.segment_minutes = 1;
		st.clip_storage = rmr_storage_create(&ccfg);
		if (!st.clip_storage)
			RSS_WARN("clip storage init failed — motion clips disabled");
	}

	/* Start continuous recording for 'continuous' and 'both' modes */
	if (st.mode != RMR_MODE_MOTION)
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
	if (st.clip_storage)
		rmr_storage_destroy(st.clip_storage);
	if (st.storage)
		rmr_storage_destroy(st.storage);
	if (st.video_ring)
		rss_ring_close(st.video_ring);
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

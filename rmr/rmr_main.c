/*
 * rmr_main.c -- Raptor Media Recorder
 *
 * Reads H.264/H.265 video + audio from SHM rings and writes
 * fragmented MP4 files to SD card. Single-threaded: the main loop
 * reads rings, feeds the muxer, and writes directly to disk.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <getopt.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>

#include "rmr.h"

/* ── Direct write callback for muxer ── */

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

/* ── Segment management ── */

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

	/* Set video params */
	rmr_video_params_t vp = {
		.codec = (st->video_codec == 1) ? RMR_CODEC_H265 : RMR_CODEC_H264,
		.width = st->width,
		.height = st->height,
		.timescale = 90000,
	};
	rmr_mux_set_video(st->mux, &vp, st->params.sps, st->params.sps_len, st->params.pps,
			  st->params.pps_len, st->params.vps_len > 0 ? st->params.vps : NULL,
			  st->params.vps_len);

	/* Set audio params if enabled */
	if (st->audio_ring) {
		rmr_audio_params_t ap = {
			.sample_rate = st->audio_sample_rate,
			.channels = 1,
		};
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
		rmr_mux_set_audio(st->mux, &ap);
	}

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

/* ── Control socket handler ── */

static int rmr_ctrl_handler(const char *cmd_json, char *resp_buf, int resp_buf_size, void *userdata)
{
	rmr_state_t *st = userdata;

	if (strstr(cmd_json, "\"start\"")) {
		atomic_store(&st->recording, true);
		snprintf(resp_buf, resp_buf_size, "{\"status\":\"ok\",\"recording\":true}");
		return (int)strlen(resp_buf);
	}

	if (strstr(cmd_json, "\"stop\"")) {
		atomic_store(&st->recording, false);
		snprintf(resp_buf, resp_buf_size, "{\"status\":\"ok\",\"recording\":false}");
		return (int)strlen(resp_buf);
	}

	if (strstr(cmd_json, "\"status\"")) {
		snprintf(resp_buf, resp_buf_size,
			 "{\"recording\":%s,\"file\":\"%s\",\"frames\":%" PRIu64
			 ",\"dropped\":%" PRIu64 ",\"bytes\":%" PRIu64 "}",
			 atomic_load(&st->recording) ? "true" : "false", st->segment_path,
			 st->frames_written, st->frames_dropped, st->bytes_written);
		return (int)strlen(resp_buf);
	}

	snprintf(resp_buf, resp_buf_size, "{\"status\":\"ok\"}");
	return (int)strlen(resp_buf);
}

/* ── Main loop ── */

static void record_loop(rmr_state_t *st)
{
	int64_t v_ts_base = -1; /* first video timestamp (for relative DTS) */
	/* Audio DTS increment per ring frame.
	 * PCM: alen / bytes_per_sample = sample count.
	 * AAC: always 1024 samples per frame (AAC-LC).
	 * Opus: 20ms at actual sample_rate = sr/50.
	 * For PCM codecs we compute from byte length; for compressed we use fixed. */
	uint32_t audio_bps = (st->audio_codec == RMR_AUDIO_PCMU || st->audio_codec == RMR_AUDIO_PCMA) ? 1 : 2;
	uint32_t audio_samples_per_frame = 0; /* 0 = derive from byte length (PCM) */
	if (st->audio_codec == RMR_AUDIO_AAC)
		audio_samples_per_frame = 1024;
	else if (st->audio_codec == RMR_AUDIO_OPUS)
		audio_samples_per_frame = st->audio_sample_rate / 50; /* 20ms */
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

		bool rec = atomic_load(&st->recording);

		/* Handle stop transition */
		if (was_recording && !rec) {
			close_segment(st);
			was_recording = false;
			RSS_INFO("recording stopped");
			continue;
		}

		if (!rec) {
			usleep(100000);
			continue;
		}

		/* Wait for storage */
		if (!rmr_storage_available(st->storage)) {
			usleep(1000000);
			continue;
		}

		/* Read video frames — drain all available before sleeping.
		 * This prevents falling behind when the loop is slow. */
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
			/* No data — wait briefly */
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
				if (start_segment(st) < 0) {
					usleep(1000000);
					continue;
				}
				v_ts_base = -1; /* reset timestamp base for new segment */
				a_dts_counter = 0;
				st->frames_written = 0;
				st->bytes_written = 0;
				was_recording = true;
			} else {
				continue; /* wait for keyframe */
			}
		}

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
			if (nal_len + 4 > (uint32_t)avcc_len) {
				RSS_WARN("AVCC corrupt: nal_len=%u avcc_len=%d src_len=%u "
					 "src[0..3]=%02x%02x%02x%02x",
					 nal_len, avcc_len, length,
					 st->frame_buf[0], st->frame_buf[1],
					 st->frame_buf[2], st->frame_buf[3]);
				continue; /* drop corrupt frame */
			}
		}

		/* DTS from actual ring timestamp (microseconds → 90kHz) */
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

		if (rmr_mux_write_video(st->mux, &vs) < 0) {
			st->frames_dropped++;
			continue;
		}
		st->frames_written++;

		/* Drain audio ring (burst read, non-blocking) */
		if (st->audio_ring) {
			for (int burst = 0; burst < 4; burst++) {
				uint32_t alen;
				rss_ring_slot_t ameta;
				ret = rss_ring_read(st->audio_ring, &st->audio_read_seq,
						    st->audio_buf, sizeof(st->audio_buf), &alen,
						    &ameta);
				if (ret == RSS_EOVERFLOW)
					break;
				if (ret != 0)
					break;

				rmr_audio_sample_t as = {
					.data = st->audio_buf,
					.size = alen,
					.dts = a_dts_counter,
				};
				rmr_mux_write_audio(st->mux, &as);
				a_dts_counter += audio_samples_per_frame
							 ? audio_samples_per_frame
							 : alen / audio_bps;
			}
		}
	}

	/* Shutdown: close any open segment */
	if (st->mux)
		close_segment(st);
}

/* ── Entry point ── */

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"  -c <file>   Config file (default: /etc/raptor.conf)\n"
		"  -f          Run in foreground\n"
		"  -d          Debug logging\n"
		"  -h          Show this help\n",
		prog);
}

int main(int argc, char **argv)
{
	const char *config_path = "/etc/raptor.conf";
	bool foreground = false;
	bool debug = false;
	int opt;

	while ((opt = getopt(argc, argv, "c:fdh")) != -1) {
		switch (opt) {
		case 'c':
			config_path = optarg;
			break;
		case 'f':
			foreground = true;
			break;
		case 'd':
			debug = true;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	rss_log_init("rmr", debug ? RSS_LOG_DEBUG : RSS_LOG_INFO,
		     foreground ? RSS_LOG_TARGET_STDERR : RSS_LOG_TARGET_SYSLOG, NULL);

	rss_config_t *cfg = rss_config_load(config_path);
	if (!cfg) {
		RSS_FATAL("failed to load config: %s", config_path);
		return 1;
	}

	if (!rss_config_get_bool(cfg, "recording", "enabled", false)) {
		RSS_INFO("recording disabled in config");
		rss_config_free(cfg);
		return 0;
	}

	if (rss_daemonize("rmr", foreground) < 0) {
		RSS_FATAL("daemonize failed");
		rss_config_free(cfg);
		return 1;
	}

	volatile sig_atomic_t *running = rss_signal_init();
	RSS_INFO("rmr starting");

	rmr_state_t st = {0};
	st.cfg = cfg;
	st.config_path = config_path;
	st.running = running;
	st.segment_fd = -1;
	st.stream_idx = rss_config_get_int(cfg, "recording", "stream", 0);
	st.audio_enabled = rss_config_get_bool(cfg, "recording", "audio", true);

	/* Open video ring */
	const char *ring_names[] = {"main", "sub"};
	const char *ring_name = ring_names[st.stream_idx < 2 ? st.stream_idx : 0];

	for (int attempt = 0; attempt < 30 && *running; attempt++) {
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

	/* Storage */
	rmr_storage_config_t scfg = {
		.base_path = rss_config_get_str(cfg, "recording", "storage_path",
						"/mnt/mmcblk0p1/raptor"),
		.segment_minutes = rss_config_get_int(cfg, "recording", "segment_minutes", 5),
		.max_storage_mb = rss_config_get_int(cfg, "recording", "max_storage_mb", 0),
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

	/* Start recording immediately */
	atomic_store(&st.recording, true);

	/* Run main loop */
	record_loop(&st);

	RSS_INFO("rmr shutting down");

cleanup:
	if (st.ctrl)
		rss_ctrl_destroy(st.ctrl);
	if (st.storage)
		rmr_storage_destroy(st.storage);
	if (st.video_ring)
		rss_ring_close(st.video_ring);
	if (st.audio_ring)
		rss_ring_close(st.audio_ring);
	free(st.frame_buf);
	free(st.avcc_buf);
	rss_config_free(cfg);

	rss_daemon_cleanup("rmr");

	return 0;
}

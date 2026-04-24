/*
 * rsp_main.c -- Raptor Stream Push
 *
 * Reads H.264/H.265 video + audio from SHM rings and pushes
 * to an RTMP/RTMPS server (YouTube Live, Twitch, etc).
 * Single-threaded: the main loop reads rings and sends RTMP
 * messages directly.
 *
 * Reconnects automatically on connection loss with configurable
 * backoff delay.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/select.h>

#include "rsp.h"
#include "rmr_nal.h"

/* ── Control socket handler ── */

static int rsp_ctrl_handler(const char *cmd_json, char *resp_buf, int resp_buf_size, void *userdata)
{
	rsp_state_t *st = userdata;

	int common =
		rss_ctrl_handle_common(cmd_json, resp_buf, resp_buf_size, st->cfg, st->config_path);
	if (common >= 0)
		return common;

	char cmd[64];
	if (rss_json_get_str(cmd_json, "cmd", cmd, sizeof(cmd)) != 0)
		return rss_ctrl_resp_error(resp_buf, resp_buf_size, "missing cmd");

	if (strcmp(cmd, "start") == 0) {
		atomic_store(&st->pushing, true);
		return rss_ctrl_resp_ok(resp_buf, resp_buf_size);
	}

	if (strcmp(cmd, "stop") == 0) {
		atomic_store(&st->pushing, false);
		return rss_ctrl_resp_ok(resp_buf, resp_buf_size);
	}

	if (strcmp(cmd, "status") == 0) {
		cJSON *r = cJSON_CreateObject();
		cJSON_AddBoolToObject(r, "pushing", atomic_load(&st->pushing));
		cJSON_AddStringToObject(r, "state",
					st->rtmp.state == RSP_STATE_PUBLISHING	   ? "publishing"
					: st->rtmp.state == RSP_STATE_CONNECTED	   ? "connected"
					: st->rtmp.state == RSP_STATE_DISCONNECTED ? "disconnected"
										   : "connecting");
		cJSON_AddStringToObject(r, "url", st->url);
		cJSON_AddNumberToObject(r, "video_frames", (double)st->frames_sent);
		cJSON_AddNumberToObject(r, "audio_frames", (double)st->audio_frames_sent);
		cJSON_AddNumberToObject(r, "dropped", (double)st->frames_dropped);
		cJSON_AddNumberToObject(r, "bytes", (double)st->bytes_sent);
		if (st->connect_time_us > 0) {
			int64_t uptime = (rss_timestamp_us() - st->connect_time_us) / 1000000;
			cJSON_AddNumberToObject(r, "uptime_sec", (double)uptime);
		}
		return rss_ctrl_resp_json(resp_buf, resp_buf_size, r);
	}

	return rss_ctrl_resp_error(resp_buf, resp_buf_size, "unknown command");
}

/* ── RTMP connection management ── */

static int rsp_connect(rsp_state_t *st)
{
	RSS_INFO("connecting to %s:%d (tls=%d)", st->host, st->port, st->use_tls);

	rss_strlcpy(st->rtmp.app, st->app, sizeof(st->rtmp.app));
	rss_strlcpy(st->rtmp.stream_key, st->stream_key, sizeof(st->rtmp.stream_key));
	st->rtmp.tcp_sndbuf = st->tcp_sndbuf;
	snprintf(st->rtmp.tc_url, sizeof(st->rtmp.tc_url), "%s://%s:%d/%s",
		 st->use_tls ? "rtmps" : "rtmp", st->host, st->port, st->app);

	if (rsp_rtmp_connect(&st->rtmp, st->host, st->port, st->use_tls) < 0)
		return -1;

	if (rsp_rtmp_publish(&st->rtmp) < 0) {
		rsp_rtmp_disconnect(&st->rtmp);
		return -1;
	}

	st->header_sent = false;
	st->connect_time_us = rss_timestamp_us();
	st->rtmp.video_ts_set = false;
	st->rtmp.audio_ts_set = false;
	return 0;
}

static void rsp_disconnect(rsp_state_t *st)
{
	rsp_rtmp_disconnect(&st->rtmp);
	st->header_sent = false;
	st->connect_time_us = 0;
}

static int rsp_send_headers(rsp_state_t *st)
{
	int video_codec = (st->video_codec == 1) ? RSP_FLV_VIDEO_H265 : RSP_FLV_VIDEO_H264;

	if (rsp_rtmp_send_video_header(&st->rtmp, video_codec, st->params.sps, st->params.sps_len,
				       st->params.pps, st->params.pps_len, st->params.vps,
				       st->params.vps_len) < 0)
		return -1;

	if (st->audio_ring) {
		uint32_t aac_rate = st->audio_enc ? 48000 : st->audio_sample_rate;
		if (rsp_rtmp_send_audio_header(&st->rtmp, aac_rate, 1) < 0)
			return -1;
	}

	st->header_sent = true;
	RSS_INFO("sequence headers sent (%s, %ux%u)", video_codec ? "H.265" : "H.264", st->width,
		 st->height);
	return 0;
}

/* ── Audio transcode callback ── */

static int rsp_audio_send_cb(const uint8_t *aac_data, uint32_t aac_len, uint32_t timestamp_ms,
			     void *userdata)
{
	rsp_state_t *st = userdata;
	int ret = rsp_rtmp_send_audio(&st->rtmp, aac_data, aac_len, timestamp_ms);
	if (ret == 0) {
		st->audio_frames_sent++;
		st->bytes_sent += aac_len;
	}
	return ret;
}

/* ── Main push loop ── */

static void push_loop(rsp_state_t *st)
{
	int ctrl_fd = st->ctrl ? rss_ctrl_get_fd(st->ctrl) : -1;
	int audio_retry = 0;
	uint64_t last_video_ws = 0;
	int video_idle = 0;
	bool was_pushing = false;
	int reconnect_wait = 0;

	while (*st->running) {
		/* Handle control socket */
		if (ctrl_fd >= 0) {
			fd_set fds;
			struct timeval tv = {0, 0};
			FD_ZERO(&fds);
			FD_SET(ctrl_fd, &fds);
			if (select(ctrl_fd + 1, &fds, NULL, NULL, &tv) > 0)
				rss_ctrl_accept_and_handle(st->ctrl, rsp_ctrl_handler, st);
		}

		bool push = atomic_load(&st->pushing);

		/* Handle stop transition */
		if (was_pushing && !push) {
			rsp_disconnect(st);
			was_pushing = false;
			reconnect_wait = 0;
			RSS_INFO("push stopped");
		}

		if (!push) {
			usleep(200000);
			continue;
		}

		/* Reconnect backoff */
		if (reconnect_wait > 0) {
			usleep(200000);
			reconnect_wait -= 200;
			if (reconnect_wait > 0)
				continue;
		}

		/* Connect if needed */
		if (st->rtmp.state < RSP_STATE_PUBLISHING) {
			if (rsp_connect(st) < 0) {
				reconnect_wait = st->reconnect_delay;
				RSS_WARN("reconnecting in %d ms", reconnect_wait);
				continue;
			}
			was_pushing = true;
		}

		/* Lazy audio ring attach */
		if (st->audio_enabled && !st->audio_ring && ++audio_retry >= 50) {
			audio_retry = 0;
			st->audio_ring = rss_ring_open("audio");
			if (st->audio_ring) {
				rss_ring_check_version(st->audio_ring, "audio");
				const rss_ring_header_t *ahdr = rss_ring_get_header(st->audio_ring);
				st->audio_codec = ahdr->codec;
				st->audio_sample_rate = ahdr->fps_num;
				st->audio_read_seq = atomic_load(&ahdr->write_seq);
				RSS_DEBUG("audio ring attached: codec=%u rate=%u", st->audio_codec,
					  st->audio_sample_rate);

				/* Init transcoder if needed */
				if (rsp_audio_needs_transcode(st->audio_codec,
							      st->audio_sample_rate) &&
				    !st->audio_enc) {
					st->audio_enc = rsp_audio_init(st->audio_codec,
								       st->audio_sample_rate);
				}

				/* Send audio header if already connected */
				if (st->header_sent) {
					uint32_t rate =
						st->audio_enc ? 48000 : st->audio_sample_rate;
					rsp_rtmp_send_audio_header(&st->rtmp, rate, 1);
				}
			}
		}

		/* Reconnect video ring if RVD restarted */
		if (!st->video_ring) {
			st->video_ring = rss_ring_open(st->video_ring_name);
			if (st->video_ring) {
				rss_ring_check_version(st->video_ring, "video");
				const rss_ring_header_t *vhdr = rss_ring_get_header(st->video_ring);
				st->use_zerocopy = (vhdr->flags & RSS_RING_FLAG_REFMODE) != 0;
				uint32_t mfs = rss_ring_max_frame_size(st->video_ring);
				if (mfs > st->avcc_buf_size) {
					uint8_t *new_avcc = malloc(mfs);
					if (!new_avcc) {
						rss_ring_close(st->video_ring);
						st->video_ring = NULL;
						continue;
					}
					free(st->avcc_buf);
					st->avcc_buf = new_avcc;
					st->avcc_buf_size = mfs;
				}
				if (!st->use_zerocopy && mfs > st->frame_buf_size) {
					uint8_t *new_frame = malloc(mfs);
					if (!new_frame) {
						rss_ring_close(st->video_ring);
						st->video_ring = NULL;
						continue;
					}
					free(st->frame_buf);
					st->frame_buf = new_frame;
					st->frame_buf_size = mfs;
				}
				st->video_codec = vhdr->codec;
				st->video_read_seq = 0;
				st->params.ready = false;
				st->header_sent = false;
				video_idle = 0;
				last_video_ws = 0;
				RSS_DEBUG("video ring reconnected (%s%s)", st->video_ring_name,
					  st->use_zerocopy ? ", zero-copy" : "");
			} else {
				usleep(200000);
			}
			continue;
		}

		/* Read video frame — peek (zero-copy) in refmode, copy otherwise */
		uint32_t length;
		rss_ring_slot_t meta;
		const uint8_t *frame_data;
		bool peeked = false;
		int ret;

		if (st->use_zerocopy) {
			ret = rss_ring_peek(st->video_ring, &st->video_read_seq, &frame_data,
					    &length, &meta);
			peeked = (ret == 0);
		} else {
			ret = rss_ring_read(st->video_ring, &st->video_read_seq, st->frame_buf,
					    st->frame_buf_size, &length, &meta);
			frame_data = st->frame_buf;
		}

		if (ret == RSS_EOVERFLOW) {
			rss_ring_request_idr(st->video_ring);
			st->frames_dropped++;
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
			if (video_idle >= 50) {
				RSS_DEBUG("video ring idle, closing (%s)", st->video_ring_name);
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

		/* Extract codec params from keyframe */
		if (meta.is_key && !st->params.ready)
			rmr_extract_params(frame_data, length, st->video_codec, &st->params);
		if (!st->params.ready) {
			if (peeked)
				rss_ring_peek_done(st->video_ring, &meta);
			continue;
		}

		/* Send sequence headers on first keyframe */
		if (!st->header_sent && meta.is_key) {
			if (rsp_send_headers(st) < 0) {
				if (peeked)
					rss_ring_peek_done(st->video_ring, &meta);
				RSS_WARN("header send failed, reconnecting");
				rsp_disconnect(st);
				reconnect_wait = st->reconnect_delay;
				continue;
			}
		}
		if (!st->header_sent) {
			if (peeked)
				rss_ring_peek_done(st->video_ring, &meta);
			continue;
		}

		/* Convert Annex B to AVCC — then release the peek */
		int avcc_len = rmr_annexb_to_avcc(frame_data, length, st->avcc_buf,
						  st->avcc_buf_size, st->video_codec);
		if (peeked)
			rss_ring_peek_done(st->video_ring, &meta);
		if (avcc_len <= 0)
			continue;

		/* Compute RTMP timestamp (ms relative to stream start) */
		if (!st->rtmp.video_ts_set) {
			st->rtmp.video_ts_base = (uint32_t)(meta.timestamp / 1000);
			st->rtmp.video_ts_set = true;
		}
		uint32_t v_ts = (uint32_t)(meta.timestamp / 1000) - st->rtmp.video_ts_base;

		int video_codec = (st->video_codec == 1) ? RSP_FLV_VIDEO_H265 : RSP_FLV_VIDEO_H264;
		if (rsp_rtmp_send_video(&st->rtmp, st->avcc_buf, (uint32_t)avcc_len, v_ts,
					meta.is_key, video_codec) < 0) {
			RSS_WARN("video send failed, reconnecting");
			rsp_disconnect(st);
			reconnect_wait = st->reconnect_delay;
			continue;
		}
		st->frames_sent++;
		st->bytes_sent += (uint32_t)avcc_len;
		if ((st->frames_sent % 250) == 0)
			RSS_DEBUG("stats: v=%" PRIu64 " a=%" PRIu64 " drop=%" PRIu64
				  " bytes=%" PRIu64,
				  st->frames_sent, st->audio_frames_sent, st->frames_dropped,
				  st->bytes_sent);

		/* Read and send audio frames */
		if (st->audio_ring) {
			for (int burst = 0; burst < 4; burst++) {
				uint32_t alen;
				rss_ring_slot_t ameta;
				ret = rss_ring_read(st->audio_ring, &st->audio_read_seq,
						    st->audio_buf, sizeof(st->audio_buf), &alen,
						    &ameta);
				if (ret != 0)
					break;

				if (!st->rtmp.audio_ts_set) {
					st->rtmp.audio_ts_base = (uint32_t)(ameta.timestamp / 1000);
					st->rtmp.audio_ts_set = true;
				}
				uint32_t a_ts =
					(uint32_t)(ameta.timestamp / 1000) - st->rtmp.audio_ts_base;

				if (st->audio_enc) {
					/* Transcode path */
					if (rsp_audio_transcode(st->audio_enc, st->audio_buf, alen,
								a_ts, rsp_audio_send_cb, st) < 0) {
						RSS_WARN("audio send failed, reconnecting");
						rsp_disconnect(st);
						reconnect_wait = st->reconnect_delay;
						break;
					}
				} else {
					/* Passthrough (already AAC) */
					if (rsp_rtmp_send_audio(&st->rtmp, st->audio_buf, alen,
								a_ts) < 0) {
						RSS_WARN("audio send failed, reconnecting");
						rsp_disconnect(st);
						reconnect_wait = st->reconnect_delay;
						break;
					}
					st->audio_frames_sent++;
					st->bytes_sent += alen;
				}
			}
		}
	}

	/* Shutdown */
	rsp_disconnect(st);
}

/* ── Entry point ── */

int main(int argc, char **argv)
{
	rss_daemon_ctx_t dctx;
	int ret = rss_daemon_init(&dctx, "rsp", argc, argv, NULL);
	if (ret != 0)
		return ret < 0 ? 1 : 0;

	if (!rss_config_get_bool(dctx.cfg, "push", "enabled", false)) {
		RSS_INFO("stream push disabled in config");
		rss_config_free(dctx.cfg);
		rss_daemon_cleanup("rsp");
		return 0;
	}

	rsp_state_t st = {0};
	st.cfg = dctx.cfg;
	st.config_path = dctx.config_path;
	st.running = dctx.running;
	st.rtmp.fd = -1;

	/* Parse URL */
	const char *url = rss_config_get_str(dctx.cfg, "push", "url", "");
	if (url[0] == '\0') {
		RSS_FATAL("no push URL configured ([push] url=)");
		goto cleanup;
	}
	rss_strlcpy(st.url, url, sizeof(st.url));

	if (rsp_rtmp_parse_url(st.url, st.host, sizeof(st.host), &st.port, st.app, sizeof(st.app),
			       st.stream_key, sizeof(st.stream_key), &st.use_tls) < 0) {
		RSS_FATAL("invalid push URL: %s", st.url);
		goto cleanup;
	}

	RSS_INFO("target: %s:%d app=%s tls=%d", st.host, st.port, st.app, st.use_tls);

	st.stream_idx = rss_config_get_int(dctx.cfg, "push", "stream", 0);
	st.audio_enabled = rss_config_get_bool(dctx.cfg, "push", "audio", true);
	st.reconnect_delay = rss_config_get_int(dctx.cfg, "push", "reconnect_ms", 5000);
	if (st.reconnect_delay < 1000)
		st.reconnect_delay = 1000;
	if (st.reconnect_delay > 60000)
		st.reconnect_delay = 60000;
	st.tcp_sndbuf = rss_config_get_int(dctx.cfg, "push", "tcp_sndbuf", 256 * 1024);

	/* Open video ring */
	static const char *ring_names[] = {"main", "sub", "s1_main", "s1_sub", "s2_main", "s2_sub"};
	int ri = st.stream_idx;
	if (ri < 0 || ri >= (int)(sizeof(ring_names) / sizeof(ring_names[0])))
		ri = 0;
	st.video_ring_name = ring_names[ri];

	for (int attempt = 0; attempt < 30 && *st.running; attempt++) {
		st.video_ring = rss_ring_open(st.video_ring_name);
		if (st.video_ring)
			break;
		RSS_DEBUG("waiting for %s ring...", st.video_ring_name);
		sleep(1);
	}

	if (!st.video_ring) {
		RSS_FATAL("video ring not available");
		goto cleanup;
	}
	rss_ring_check_version(st.video_ring, "video");

	/* Read ring metadata */
	const rss_ring_header_t *vhdr = rss_ring_get_header(st.video_ring);
	st.video_codec = vhdr->codec;
	st.width = vhdr->width;
	st.height = vhdr->height;
	st.fps_num = vhdr->fps_num;
	st.use_zerocopy = (vhdr->flags & RSS_RING_FLAG_REFMODE) != 0;
	RSS_INFO("video: %s %ux%u @ %u fps%s", st.video_codec == 1 ? "H.265" : "H.264", st.width,
		 st.height, st.fps_num, st.use_zerocopy ? " (zero-copy)" : "");

	/* Allocate buffers — frame_buf only needed in copy mode */
	st.frame_buf_size = rss_ring_max_frame_size(st.video_ring);
	st.frame_buf = st.use_zerocopy ? NULL : malloc(st.frame_buf_size);
	st.avcc_buf_size = st.frame_buf_size;
	st.avcc_buf = malloc(st.avcc_buf_size);
	if ((!st.use_zerocopy && !st.frame_buf) || !st.avcc_buf) {
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
			RSS_INFO("audio: codec=%u rate=%u", st.audio_codec, st.audio_sample_rate);
			if (rsp_audio_needs_transcode(st.audio_codec, st.audio_sample_rate)) {
				st.audio_enc = rsp_audio_init(st.audio_codec, st.audio_sample_rate);
				if (!st.audio_enc)
					RSS_WARN("audio transcode init failed, audio disabled");
			}
		} else {
			RSS_WARN("audio ring not available (pushing video only)");
		}
	}

	/* Control socket */
	rss_mkdir_p(RSS_RUN_DIR);
	st.ctrl = rss_ctrl_listen(RSS_RUN_DIR "/rsp.sock");
	if (!st.ctrl)
		RSS_WARN("control socket failed (non-fatal)");

	/* Auto-start if configured */
	bool autostart = rss_config_get_bool(dctx.cfg, "push", "autostart", true);
	atomic_store(&st.pushing, autostart);
	if (autostart)
		RSS_INFO("autostart enabled, connecting...");
	else
		RSS_INFO("waiting for start command");

	/* Run main loop */
	push_loop(&st);

	RSS_INFO("rsp shutting down");

cleanup:
	if (st.ctrl)
		rss_ctrl_destroy(st.ctrl);
	if (st.video_ring)
		rss_ring_close(st.video_ring);
	if (st.audio_ring)
		rss_ring_close(st.audio_ring);
	if (st.audio_enc)
		rsp_audio_free(st.audio_enc);
	free(st.frame_buf);
	free(st.avcc_buf);
	rss_config_free(dctx.cfg);

	rss_daemon_cleanup("rsp");

	return 0;
}

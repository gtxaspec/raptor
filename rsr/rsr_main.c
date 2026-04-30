/*
 * rsr_main.c — Raptor SRT Listener
 *
 * Reads H.264/H.265 video + audio from SHM rings, muxes into
 * MPEG-TS, and serves to SRT clients. Supports multiple clients
 * on different streams simultaneously via SRT STREAMID.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <poll.h>
#include <arpa/inet.h>

#include "rsr.h"

/* ── Stream management ── */

rsr_stream_t *rsr_stream_get_or_open(rsr_state_t *st, const char *name)
{
	/* Find existing active stream */
	for (int i = 0; i < RSR_MAX_STREAMS; i++) {
		if (st->streams[i].active && strcmp(st->streams[i].name, name) == 0)
			return &st->streams[i];
	}

	/* Validate ring name */
	int idx = -1;

	for (int i = 0; i < RSR_MAX_STREAMS; i++) {
		if (strcmp(rsr_ring_names[i], name) == 0) {
			idx = i;
			break;
		}
	}
	if (idx < 0)
		return NULL;

	/* Find empty slot */
	rsr_stream_t *s = NULL;

	for (int i = 0; i < RSR_MAX_STREAMS; i++) {
		if (!st->streams[i].active) {
			s = &st->streams[i];
			break;
		}
	}
	if (!s)
		return NULL;

	/* Open ring */
	rss_ring_t *ring = rss_ring_open(name);

	if (!ring) {
		RSS_WARN("ring '%s' not available", name);
		return NULL;
	}
	rss_ring_check_version(ring, name);

	const rss_ring_header_t *hdr = rss_ring_get_header(ring);

	s->ring = ring;
	s->name = rsr_ring_names[idx];
	s->read_seq = 0;
	s->codec = hdr->codec;
	s->width = hdr->width;
	s->height = hdr->height;
	s->client_count = 0;
	s->active = true;

	rss_ring_acquire(ring);

	/* Ensure frame buffer is big enough for this ring */
	uint32_t mfs = rss_ring_max_frame_size(ring);

	if (mfs > st->frame_buf_size) {
		uint8_t *nb = realloc(st->frame_buf, mfs);

		if (!nb) {
			rss_ring_release(ring);
			rss_ring_close(ring);
			s->active = false;
			return NULL;
		}
		st->frame_buf = nb;
		st->frame_buf_size = mfs;

		/* TS buffer needs ~(mfs/184 + 2) * 188 */
		uint32_t ts_need = ((mfs / 184) + 4) * 188;

		if (ts_need > st->ts_buf_size) {
			uint8_t *tb = realloc(st->ts_buf, ts_need);

			if (!tb) {
				rss_ring_release(ring);
				rss_ring_close(ring);
				s->active = false;
				return NULL;
			}
			st->ts_buf = tb;
			st->ts_buf_size = ts_need;
		}
	}

	RSS_INFO("stream '%s' opened: %s %ux%u", name, s->codec == 1 ? "H.265" : "H.264", s->width,
		 s->height);

	return s;
}

void rsr_stream_release(rsr_state_t *st, rsr_stream_t *s)
{
	(void)st;

	if (!s->active)
		return;

	RSS_INFO("stream '%s' closed (no clients)", s->name);
	rss_ring_release(s->ring);
	rss_ring_close(s->ring);
	s->ring = NULL;
	s->active = false;
}

/* ── Audio codec to TS stream type ── */

static uint8_t audio_codec_to_ts(uint32_t codec)
{
	switch (codec) {
	case 4:
		return RSS_TS_STREAM_AAC;
	case 6:
		return RSS_TS_STREAM_OPUS;
	default:
		return RSS_TS_STREAM_NONE;
	}
}

/* ── Control socket handler ── */

static int rsr_ctrl_handler(const char *cmd_json, char *resp_buf, int resp_buf_size, void *userdata)
{
	rsr_state_t *st = userdata;

	int common =
		rss_ctrl_handle_common(cmd_json, resp_buf, resp_buf_size, st->cfg, st->config_path);

	if (common >= 0)
		return common;

	char cmd[64];

	if (rss_json_get_str(cmd_json, "cmd", cmd, sizeof(cmd)) != 0)
		return rss_ctrl_resp_error(resp_buf, resp_buf_size, "missing cmd");

	if (strcmp(cmd, "status") == 0) {
		cJSON *r = cJSON_CreateObject();

		if (!r)
			return rss_ctrl_resp_error(resp_buf, resp_buf_size, "alloc");

		cJSON_AddNumberToObject(r, "port", st->port);
		cJSON_AddNumberToObject(r, "clients", st->client_count);
		cJSON_AddNumberToObject(r, "max_clients", st->max_clients);
		cJSON_AddNumberToObject(r, "latency_ms", st->latency_ms);
		cJSON_AddBoolToObject(r, "encrypted", st->passphrase[0] != '\0');
		cJSON_AddNumberToObject(r, "total_frames", (double)st->total_frames);
		cJSON_AddNumberToObject(r, "total_bytes", (double)st->total_bytes);

		int active_streams = 0;

		for (int i = 0; i < RSR_MAX_STREAMS; i++) {
			if (st->streams[i].active)
				active_streams++;
		}
		cJSON_AddNumberToObject(r, "active_streams", active_streams);

		return rss_ctrl_resp_json(resp_buf, resp_buf_size, r);
	}

	if (strcmp(cmd, "clients") == 0) {
		cJSON *r = cJSON_CreateObject();

		if (!r)
			return rss_ctrl_resp_error(resp_buf, resp_buf_size, "alloc");

		cJSON *arr = cJSON_AddArrayToObject(r, "clients");

		for (int i = 0; i < st->client_count; i++) {
			rsr_client_t *c = &st->clients[i];
			cJSON *obj = cJSON_CreateObject();

			if (!obj)
				continue;

			char addr_str[64];

			rsr_client_addr_str(&c->addr, addr_str, sizeof(addr_str));

			cJSON_AddStringToObject(obj, "addr", addr_str);
			cJSON_AddStringToObject(obj, "stream", c->stream ? c->stream->name : "?");
			cJSON_AddNumberToObject(obj, "frames", (double)c->frames_sent);
			cJSON_AddNumberToObject(obj, "bytes", (double)c->bytes_sent);

			if (c->connect_time_us > 0) {
				int64_t up = (rss_timestamp_us() - c->connect_time_us) / 1000000;

				cJSON_AddNumberToObject(obj, "uptime_sec", (double)up);
			}
			cJSON_AddBoolToObject(obj, "waiting_keyframe", c->waiting_keyframe);
			cJSON_AddItemToArray(arr, obj);
		}

		return rss_ctrl_resp_json(resp_buf, resp_buf_size, r);
	}

	return rss_ctrl_resp_error(resp_buf, resp_buf_size, "unknown command");
}

/* ── Main loop ── */

static void serve_loop(rsr_state_t *st)
{
	uint8_t audio_buf[8192];

	while (rss_running(st->running)) {
		/* Poll SRT for new connections and errors */
		rsr_srt_poll(st);

		/* Handle control socket (non-blocking check) */
		if (st->ctrl) {
			struct pollfd pfd = {.fd = rss_ctrl_get_fd(st->ctrl), .events = POLLIN};

			if (poll(&pfd, 1, 0) > 0)
				rss_ctrl_accept_and_handle(st->ctrl, rsr_ctrl_handler, st);
		}

		/* No clients — idle */
		if (st->client_count == 0) {
			usleep(200000);
			continue;
		}

		/* Reconnect audio ring if needed */
		if (st->audio_enabled && !st->audio_ring) {
			st->audio_ring = rss_ring_open("audio");
			if (st->audio_ring) {
				rss_ring_check_version(st->audio_ring, "audio");
				const rss_ring_header_t *ahdr = rss_ring_get_header(st->audio_ring);
				st->audio_codec = ahdr->codec;
				st->audio_ts_type = audio_codec_to_ts(ahdr->codec);
				st->audio_read_seq = ahdr->write_seq;
				rss_ring_acquire(st->audio_ring);
				RSS_INFO("audio ring attached: codec=%u", st->audio_codec);
			}
		}

		bool got_frame = false;

		/* Read from each active stream */
		for (int si = 0; si < RSR_MAX_STREAMS; si++) {
			rsr_stream_t *s = &st->streams[si];

			if (!s->active || s->client_count <= 0)
				continue;

			uint32_t length;
			rss_ring_slot_t meta;
			int ret = rss_ring_read(s->ring, &s->read_seq, st->frame_buf,
						st->frame_buf_size, &length, &meta);

			if (ret == RSS_EOVERFLOW) {
				rss_ring_request_idr(s->ring);
				/* Reset waiting_keyframe for all clients on this stream */
				for (int ci = 0; ci < st->client_count; ci++) {
					if (st->clients[ci].stream == s)
						st->clients[ci].waiting_keyframe = true;
				}
				continue;
			}
			if (ret == -EAGAIN)
				continue;
			if (ret != 0)
				continue;

			got_frame = true;
			uint64_t pts = meta.timestamp * 9 / 100;

			/* Distribute to clients on this stream */
			for (int ci = st->client_count - 1; ci >= 0; ci--) {
				rsr_client_t *c = &st->clients[ci];

				if (c->stream != s)
					continue;

				if (c->waiting_keyframe) {
					if (!meta.is_key)
						continue;
					c->waiting_keyframe = false;
				}

				/* PAT/PMT before keyframes */
				if (meta.is_key) {
					size_t pat_len = rss_ts_write_pat_pmt(&c->ts, st->ts_buf,
									      st->ts_buf_size);
					if (pat_len > 0) {
						if (rsr_srt_send_to_client(c, st->ts_buf, pat_len) <
						    0) {
							rsr_remove_client(st, ci);
							continue;
						}
					}
				}

				/* Video TS packets (DTS == PTS for I/P-only streams) */
				size_t ts_len = rss_ts_write_video(&c->ts, st->ts_buf,
								   st->ts_buf_size, st->frame_buf,
								   length, pts, pts, meta.is_key);
				if (ts_len > 0) {
					if (rsr_srt_send_to_client(c, st->ts_buf, ts_len) < 0) {
						rsr_remove_client(st, ci);
						continue;
					}
				}

				st->total_frames++;
				st->total_bytes += ts_len;
			}
		}

		/* Read and send audio (shared across all non-waiting clients) */
		if (st->audio_ring) {
			for (int burst = 0; burst < 4; burst++) {
				uint32_t alen;
				rss_ring_slot_t ameta;
				int ret =
					rss_ring_read(st->audio_ring, &st->audio_read_seq,
						      audio_buf, sizeof(audio_buf), &alen, &ameta);

				if (ret == RSS_EOVERFLOW) {
					const rss_ring_header_t *ahdr =
						rss_ring_get_header(st->audio_ring);
					st->audio_read_seq =
						ahdr->write_seq > 0 ? ahdr->write_seq - 1 : 0;
					continue;
				}
				if (ret != 0)
					break;

				uint64_t apts = ameta.timestamp * 9 / 100;

				for (int ci = st->client_count - 1; ci >= 0; ci--) {
					rsr_client_t *c = &st->clients[ci];

					if (c->waiting_keyframe)
						continue;

					size_t ats_len = rss_ts_write_audio(&c->ts, st->ts_buf,
									    st->ts_buf_size,
									    audio_buf, alen, apts);
					if (ats_len > 0) {
						if (rsr_srt_send_to_client(c, st->ts_buf, ats_len) <
						    0) {
							rsr_remove_client(st, ci);
							continue;
						}
					}
					st->total_bytes += ats_len;
				}
			}
		}

		if (!got_frame) {
			/* Wait on first active stream's ring for new data */
			for (int si = 0; si < RSR_MAX_STREAMS; si++) {
				if (st->streams[si].active && st->streams[si].client_count > 0) {
					rss_ring_wait(st->streams[si].ring, 20);
					break;
				}
			}
		}
	}
}

/* ── Entry point ── */

int main(int argc, char **argv)
{
	rss_daemon_ctx_t dctx;
	int ret = rss_daemon_init(&dctx, "rsr", argc, argv, NULL);

	if (ret != 0)
		return ret < 0 ? 1 : 0;

	if (!rss_config_get_bool(dctx.cfg, "srt", "enabled", false)) {
		RSS_INFO("SRT listener disabled in config");
		rss_config_free(dctx.cfg);
		rss_daemon_cleanup("rsr");
		return 0;
	}

	rsr_state_t st = {0};

	st.cfg = dctx.cfg;
	st.config_path = dctx.config_path;
	st.running = dctx.running;
	st.listener = SRT_INVALID_SOCK;
	st.srt_eid = -1;

	/* Read config */
	st.port = rss_config_get_int(dctx.cfg, "srt", "port", 9000);
	st.latency_ms = rss_config_get_int(dctx.cfg, "srt", "latency", 120);
	st.max_clients = rss_config_get_int(dctx.cfg, "srt", "max_clients", 4);
	st.audio_enabled = rss_config_get_bool(dctx.cfg, "srt", "audio", true);

	if (st.max_clients > RSR_MAX_CLIENTS)
		st.max_clients = RSR_MAX_CLIENTS;
	if (st.max_clients < 1)
		st.max_clients = 1;

	const char *pp = rss_config_get_str(dctx.cfg, "srt", "passphrase", "");

	rss_strlcpy(st.passphrase, pp, sizeof(st.passphrase));

	st.pbkeylen = rss_config_get_int(dctx.cfg, "srt", "pbkeylen", 16);
	if (st.pbkeylen != 16 && st.pbkeylen != 24 && st.pbkeylen != 32)
		st.pbkeylen = 16;

	st.default_stream_idx = rss_config_get_int(dctx.cfg, "srt", "stream", 0);
	if (st.default_stream_idx < 0 || st.default_stream_idx >= RSR_MAX_STREAMS)
		st.default_stream_idx = 0;

	/* Allocate initial buffers */
	st.frame_buf_size = 256 * 1024;
	st.frame_buf = malloc(st.frame_buf_size);
	st.ts_buf_size = ((st.frame_buf_size / 184) + 4) * 188;
	st.ts_buf = malloc(st.ts_buf_size);

	if (!st.frame_buf || !st.ts_buf) {
		RSS_FATAL("buffer allocation failed");
		goto cleanup;
	}

	/* Start SRT listener */
	if (rsr_srt_init(&st) < 0)
		goto cleanup;

	/* Open default video ring */
	const char *ring_name = rsr_ring_names[st.default_stream_idx];

	for (int attempt = 0; attempt < 30 && *st.running; attempt++) {
		if (rsr_stream_get_or_open(&st, ring_name))
			break;
		RSS_DEBUG("waiting for %s ring...", ring_name);
		sleep(1);
	}

	/* Open audio ring */
	if (st.audio_enabled) {
		st.audio_ring = rss_ring_open("audio");
		if (st.audio_ring) {
			rss_ring_check_version(st.audio_ring, "audio");
			const rss_ring_header_t *ahdr = rss_ring_get_header(st.audio_ring);

			st.audio_codec = ahdr->codec;
			st.audio_ts_type = audio_codec_to_ts(ahdr->codec);
			st.audio_read_seq = ahdr->write_seq;
			rss_ring_acquire(st.audio_ring);
			RSS_INFO("audio: codec=%u ts_type=0x%02x", st.audio_codec,
				 st.audio_ts_type);
		} else {
			RSS_WARN("audio ring not available");
		}
	}

	/* Control socket */
	rss_mkdir_p(RSS_RUN_DIR);
	st.ctrl = rss_ctrl_listen(RSS_RUN_DIR "/rsr.sock");
	if (!st.ctrl)
		RSS_WARN("control socket failed (non-fatal)");

	/* Run */
	serve_loop(&st);

	RSS_INFO("rsr shutting down");

cleanup:
	rsr_srt_cleanup(&st);

	if (st.audio_ring) {
		rss_ring_release(st.audio_ring);
		rss_ring_close(st.audio_ring);
	}

	for (int i = 0; i < RSR_MAX_STREAMS; i++) {
		if (st.streams[i].active)
			rsr_stream_release(&st, &st.streams[i]);
	}

	if (st.ctrl)
		rss_ctrl_destroy(st.ctrl);

	free(st.frame_buf);
	free(st.ts_buf);
	rss_config_free(dctx.cfg);
	rss_daemon_cleanup("rsr");

	return 0;
}

/*
 * rsd_ring_reader.c -- SHM ring consumer + frame distribution
 *
 * A dedicated thread reads frames from the SHM ring and distributes
 * them to all playing RTSP clients via their compy NalTransport.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include <pthread.h>

#include "rsd.h"

/* Forward declarations — called by send thread, defined below */
static void rsd_send_audio_frame(rsd_client_t *c, uint32_t codec, const uint8_t *data, uint32_t len,
				 uint32_t rtp_ts, int64_t capture_us);
static void rsd_send_jpeg_frame(rsd_client_t *c, const uint8_t *data, uint32_t len,
				uint32_t rtp_ts);

/*
 * Minimum interval between IDR requests from the reader's lag-recovery
 * paths (skip-to-latest, RSS_EOVERFLOW, sendq-full). Without this cap the
 * three call sites cascade on slow SoCs: each IDR is ~10x a P-frame, so
 * requesting one slows the reader, which triggers another skip, which
 * requests another IDR — observed as 1:9 IDR:SLICE on T20 (expected with
 * GOP=60 is 1:60). One second is enough: the encoder's own GOP will
 * produce its next IDR in due course, and the client already has the
 * current keyframe.
 */
#define RSD_IDR_REQ_MIN_INTERVAL_US 1000000

static inline void rsd_maybe_request_idr(rss_ring_t *ring, int64_t *last_req_us)
{
	int64_t now_us = rss_timestamp_us();
	if (now_us - *last_req_us > RSD_IDR_REQ_MIN_INTERVAL_US) {
		rss_ring_request_idr(ring);
		*last_req_us = now_us;
	}
}

/*
 * Extract SPS/PPS from Annex B keyframe data and cache in ring context.
 * Called once when the first keyframe is seen (or when SPS/PPS change).
 */
static void rsd_cache_params(rsd_ring_ctx_t *rctx, const uint8_t *data, uint32_t len)
{
	const uint8_t *p = data;
	const uint8_t *end = data + len;
	bool is_h265 = (atomic_load_explicit(&rctx->last_codec, memory_order_relaxed) == 1);

	while (p + 4 < end) {
		if (!(p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1)) {
			p++;
			continue;
		}

		const uint8_t *nalu_start = p + 4;
		const uint8_t *nalu_end = end;
		for (const uint8_t *q = nalu_start + 1; q + 3 < end; q++) {
			if (q[0] == 0 && q[1] == 0 && q[2] == 0 && q[3] == 1) {
				nalu_end = q;
				break;
			}
		}

		uint32_t nalu_len = (uint32_t)(nalu_end - nalu_start);
		if (nalu_len < 2) {
			p = nalu_end;
			continue;
		}

		if (is_h265) {
			uint8_t nal_type = (nalu_start[0] >> 1) & 0x3F;
			if (nal_type == 32 && nalu_len <= sizeof(rctx->vps)) {
				memcpy(rctx->vps, nalu_start, nalu_len);
				atomic_store_explicit(&rctx->vps_len, (uint16_t)nalu_len,
						      memory_order_release);
			} else if (nal_type == 33 && nalu_len <= sizeof(rctx->sps)) {
				memcpy(rctx->sps, nalu_start, nalu_len);
				atomic_store_explicit(&rctx->sps_len, (uint16_t)nalu_len,
						      memory_order_release);
			} else if (nal_type == 34 && nalu_len <= sizeof(rctx->pps)) {
				memcpy(rctx->pps, nalu_start, nalu_len);
				atomic_store_explicit(&rctx->pps_len, (uint16_t)nalu_len,
						      memory_order_release);
			}
		} else {
			uint8_t nal_type = nalu_start[0] & 0x1F;
			if (nal_type == 7 && nalu_len <= sizeof(rctx->sps)) {
				memcpy(rctx->sps, nalu_start, nalu_len);
				atomic_store_explicit(&rctx->sps_len, (uint16_t)nalu_len,
						      memory_order_release);
			} else if (nal_type == 8 && nalu_len <= sizeof(rctx->pps)) {
				memcpy(rctx->pps, nalu_start, nalu_len);
				atomic_store_explicit(&rctx->pps_len, (uint16_t)nalu_len,
						      memory_order_release);
			}
		}

		p = nalu_end;
	}
}

static const char *rsd_codec_name(uint32_t codec)
{
	switch (codec) {
	case 0:
		return "H.264";
	case 1:
		return "H.265";
	default:
		return "JPEG";
	}
}

/*
 * Drop every playing client on a stream whose SDP has stopped being true.
 *
 * Codec and picture size are both answered in the SDP — sprop-parameter-sets
 * carries the SPS, and the SPS carries the dimensions — and RTSP has no way to
 * renegotiate that mid-session. A client left in place is told 2560x1920 while
 * being sent 1280x720: some players re-read the in-band SPS and recover, others
 * show corruption or freeze, and a recorder writes a file that disagrees with
 * its own header. Disconnecting makes the client re-DESCRIBE, which is the only
 * transition RTSP actually defines.
 *
 * Everyone with a SETUP transport goes, not just the playing: a PAUSEd client
 * holds the same stale SDP and would resume straight onto the new geometry.
 */
static void rsd_drop_stream_clients(rsd_server_t *srv, int stream_idx, const char *what)
{
	pthread_mutex_lock(&srv->clients_lock);
	for (int i = 0; i < srv->client_count; i++) {
		rsd_client_t *c = srv->clients[i];
		if (c && c->stream_idx == stream_idx && c->video.rtp) {
			shutdown(c->fd, SHUT_RDWR);
			RSS_INFO("disconnecting client on stream %d (%s changed)", stream_idx,
				 what);
		}
	}
	pthread_mutex_unlock(&srv->clients_lock);
}

/* Try to pop and send one audio entry from the sendq.
 * Called between video NALUs to interleave audio with large IDR frames. */
static void sendq_drain_audio(rsd_client_t *c)
{
	rsd_sendq_t *q = &c->sendq;
	rsd_sendq_entry_t audio;
	bool got;

	pthread_mutex_lock(&q->lock);
	got = rsd_sendq_take_audio_locked(q, &audio);
	pthread_mutex_unlock(&q->lock);

	if (got) {
		pthread_mutex_lock(&c->write_lock);
		rsd_send_audio_frame(c, audio.codec, audio.data, audio.len, audio.rtp_ts,
				     audio.capture_us);
		pthread_mutex_unlock(&c->write_lock);
		rsd_sendq_release_entry(&audio);
	}
}

/*
 * Send a video frame with audio interleaving. Parses Annex B NALUs
 * and sends each one, draining queued audio between NALUs to prevent
 * large IDR frames from starving audio delivery.
 */
static void rsd_send_video_interleaved(rsd_client_t *c, const uint8_t *data, uint32_t len,
				       uint32_t rtp_ts)
{
	if (!c->video.nal || !c->video.playing)
		return;

	bool is_h265 = (c->video_codec == 1);
	int hdr_size = is_h265 ? 2 : 1;
	int nalu_count = 0;

	const uint8_t *p = data;
	const uint8_t *end = data + len;

	while (p + 4 < end) {
		if (!(p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1)) {
			p++;
			continue;
		}

		const uint8_t *nalu_start = p + 4;
		const uint8_t *nalu_end = end;
		for (const uint8_t *q = nalu_start + 1; q + 3 < end; q++) {
			if (q[0] == 0 && q[1] == 0 && q[2] == 0 && q[3] == 1) {
				nalu_end = q;
				break;
			}
		}

		uint32_t nalu_len = (uint32_t)(nalu_end - nalu_start);
		if (nalu_len < (uint32_t)hdr_size) {
			p = nalu_end;
			continue;
		}

		Compy_NalUnit nalu;
		if (is_h265) {
			nalu = (Compy_NalUnit){
				.header = Compy_NalHeader_H265(
					Compy_H265NalHeader_parse((uint8_t *)nalu_start)),
				.payload = U8Slice99_new((uint8_t *)(nalu_start + 2), nalu_len - 2),
			};
		} else {
			nalu = (Compy_NalUnit){
				.header = Compy_NalHeader_H264(
					Compy_H264NalHeader_parse(nalu_start[0])),
				.payload = U8Slice99_new((uint8_t *)(nalu_start + 1), nalu_len - 1),
			};
		}

		pthread_mutex_lock(&c->write_lock);
		(void)!Compy_NalTransport_send_packet(c->video.nal, Compy_RtpTimestamp_Raw(rtp_ts),
						      nalu);
		pthread_mutex_unlock(&c->write_lock);

		nalu_count++;
		p = nalu_end;

		/* After every ~10 video packets, drain one audio entry.
		 * 10 packets ≈ 12KB at 1200-byte MTU — enough to keep
		 * video throughput high while letting audio through. */
		if (nalu_count % 10 == 0)
			sendq_drain_audio(c);
	}

	if (c->srv->rtcp_sr) {
		int64_t now = rss_timestamp_us();
		if (c->video.rtcp && now - c->video.last_rtcp > RSD_SR_INTERVAL_US) {
			pthread_mutex_lock(&c->write_lock);
			(void)!Compy_Rtcp_send_sr(c->video.rtcp);
			pthread_mutex_unlock(&c->write_lock);
			c->video.last_rtcp = now;
		}
	}
}

/* Per-client send thread — drains sendq through compy (blocking I/O) */
void *rsd_client_send_thread(void *arg)
{
	rsd_client_t *c = arg;
	rsd_sendq_t *q = &c->sendq;

	while (1) {
		pthread_mutex_lock(&q->lock);
		while (q->count == 0 && !q->shutdown)
			pthread_cond_wait(&q->cond, &q->lock);

		if (q->shutdown) {
			pthread_mutex_unlock(&q->lock);
			break;
		}

		rsd_sendq_entry_t entry = q->entries[q->tail];
		q->entries[q->tail].data = NULL;
		q->tail = (q->tail + 1) % RSD_SENDQ_SLOTS;
		q->count--;
		pthread_mutex_unlock(&q->lock);

		if (entry.type == RSD_FRAME_VIDEO) {
			if (c->video.jpeg)
				rsd_send_jpeg_frame(c, entry.data, entry.len, entry.rtp_ts);
			else
				rsd_send_video_interleaved(c, entry.data, entry.len, entry.rtp_ts);
		} else {
			pthread_mutex_lock(&c->write_lock);
			rsd_send_audio_frame(c, entry.codec, entry.data, entry.len, entry.rtp_ts,
					     entry.capture_us);
			pthread_mutex_unlock(&c->write_lock);
		}

		rsd_sendq_release_entry(&entry);
	}

	return NULL;
}

void *rsd_video_reader_thread(void *arg)
{
	rsd_ring_ctx_t *rctx = arg;
	rsd_server_t *srv = rctx->srv;
	int stream_idx = rctx->idx;

	/* Wall-clock video timestamps: derive from IMP's CLOCK_MONOTONIC_RAW
	 * timestamp, same clock source as audio. Both streams share the same
	 * timebase so inter-stream drift is zero by construction. */
	int64_t video_ts_epoch = 0;
	uint32_t last_rtp_ts = 0; /* enforce monotonic RTP timestamps */
	bool has_last_rtp_ts = false;
	uint64_t last_write_seq = 0;
	int idle_count = 0;

	/* Per-thread state for rsd_maybe_request_idr (see top of file). */
	int64_t last_idr_req_us = 0;

	uint64_t total_read = 0, total_pushed = 0, total_overflow = 0;
	int64_t last_count_log = rss_timestamp_us();

	RSS_DEBUG("video reader[%d] started", stream_idx);
	while (rss_running(srv->running)) {
		if (!rctx->ring) {
			/* Ring lost — wait for RVD to recreate it */
			rctx->ring = rss_ring_open(rctx->ring_name);
			if (!rctx->ring) {
				usleep(200000);
				continue;
			}
			rss_ring_check_version(rctx->ring, rctx->ring_name);
			uint32_t max_frame = rss_ring_max_frame_size(rctx->ring);
			if (rctx->frame_buf_size < max_frame) {
				uint8_t *new_buf = malloc(max_frame);
				if (!new_buf) {
					rss_ring_release(rctx->ring);
					rss_ring_close(rctx->ring);
					rctx->ring = NULL;
					rctx->frame_buf_size = 0;
					continue;
				}
				free(rctx->frame_buf);
				rctx->frame_buf = new_buf;
				rctx->frame_buf_size = max_frame;
			}
			/* Register as a consumer: demand-driven producers
			 * (rvd's jpeg pulse) only encode while the ring has
			 * acquired readers; open() alone is invisible to them. */
			rss_ring_acquire(rctx->ring);
			rctx->read_seq = 0;
			last_write_seq = 0;
			idle_count = 0;
			video_ts_epoch = 0;
			last_rtp_ts = 0;
			has_last_rtp_ts = false;
			atomic_store_explicit(&rctx->vps_len, 0, memory_order_relaxed);
			atomic_store_explicit(&rctx->sps_len, 0, memory_order_relaxed);
			atomic_store_explicit(&rctx->pps_len, 0, memory_order_relaxed);

			/* Cache all SDP-relevant fields so the session thread
			 * never needs to dereference the ring pointer. The seqlock
			 * snapshot cannot hand back a torn pair; -EAGAIN (producer
			 * died mid-rewrite) keeps the old cache, and the live
			 * check below catches up on the next pass. */
			rss_stream_info_t si;
			bool codec_changed = false;
			bool geometry_changed = false;
			if (rss_ring_get_stream_info(rctx->ring, &si) == 0) {
				/* Both comparisons read what the old ring advertised,
				 * so they have to happen before the cache is
				 * overwritten. */
				codec_changed = (si.codec != rctx->last_codec);
				geometry_changed = (si.width != rctx->last_width ||
						    si.height != rctx->last_height);
				rctx->last_codec = si.codec;
				rctx->last_width = si.width;
				rctx->last_height = si.height;
				rctx->last_fps_num = si.fps_num;
				rctx->last_fps_den = si.fps_den;
				rctx->last_profile = si.profile;
				rctx->last_level = si.level;
			}

			/* Ring reconnected — reset all clients on this stream
			 * so they re-sync from the next keyframe, unless what the
			 * SDP promised them has changed, in which case they are
			 * dropped instead (see rsd_drop_stream_clients).
			 *
			 * A client can only be playing video if last_width was
			 * already nonzero -- SETUP refuses the stream otherwise
			 * (rsd_session.c) -- so the first open of a ring cannot
			 * disconnect anyone through the 0 -> real transition. */
			if (codec_changed || geometry_changed) {
				rsd_drop_stream_clients(srv, stream_idx,
							codec_changed ? "codec" : "resolution");
			} else {
				pthread_mutex_lock(&srv->clients_lock);
				for (int i = 0; i < srv->client_count; i++) {
					rsd_client_t *c = srv->clients[i];
					if (c && c->stream_idx == stream_idx && c->video.playing) {
						c->waiting_keyframe = true;
						c->video_ts_base_set = false;
					}
				}
				pthread_mutex_unlock(&srv->clients_lock);
			}

			RSS_INFO("video reader[%d] reconnected (%s%s%s)", stream_idx,
				 rctx->ring_name, codec_changed ? ", codec changed" : "",
				 geometry_changed ? ", resolution changed" : "");
		}

		int ret = rss_ring_wait(rctx->ring, 100);
		if (ret != 0) {
			const rss_ring_header_t *h = rss_ring_get_header(rctx->ring);
			uint64_t ws = h->write_seq;
			if (ws == last_write_seq)
				idle_count++;
			else
				idle_count = 0;
			last_write_seq = ws;

			/* A frozen write_seq can mean an idle encoder OR a
			 * producer that died and was reborn: the new instance
			 * is a NEW shm file, so this mapping never moves again.
			 * The idle-close below cannot fire while any client is
			 * playing, and a lingering client (stalled player that
			 * still ACKs) pinned readers to a dead ring for minutes
			 * in the field. Detect the rebirth directly. */
			if (idle_count >= 10 && rss_ring_stale(rctx->ring)) {
				RSS_WARN("video reader[%d]: ring %s replaced under us "
					 "(producer restart) -- reopening",
					 stream_idx, rctx->ring_name);
				rss_ring_release(rctx->ring);
				rss_ring_close(rctx->ring);
				rctx->ring = NULL;
				continue;
			}
			if (idle_count >= 20) {
				/* Keep the ring open while any client is playing
				 * this stream: rvd's jpeg encoder publishes only
				 * while the ring has readers (demand pulse), so
				 * idle-closing here starves a live client (rvd
				 * waits for readers, we wait for writes). */
				bool have_clients = false;
				pthread_mutex_lock(&srv->clients_lock);
				for (int i = 0; i < srv->client_count; i++) {
					rsd_client_t *c = srv->clients[i];
					if (c && c->stream_idx == stream_idx && c->video.playing) {
						have_clients = true;
						break;
					}
				}
				pthread_mutex_unlock(&srv->clients_lock);
				if (!have_clients) {
					RSS_DEBUG("video reader[%d] idle, closing ring (%s)",
						  stream_idx, rctx->ring_name);
					rss_ring_release(rctx->ring);
					rss_ring_close(rctx->ring);
					rctx->ring = NULL;
				}
				idle_count = 0;
			}
			continue;
		}
		idle_count = 0;

		if (!rctx->frame_buf)
			continue;

		/* Drain all available frames before going back to ring_wait.
		 * On single-core SoCs the scheduler may give CPU to the encoder
		 * thread before waking the reader, so 2-3 frames can accumulate
		 * between ring_wait returns. Processing them sequentially
		 * preserves every frame; the reader is fast enough to catch up
		 * (measured <2ms per frame vs 33ms budget on T20). */
		/* One seqlock snapshot serves the pacing, the rate refresh and
		 * the change check below, so all three see the same rewrite —
		 * never the new width beside the old height. -EAGAIN (producer
		 * died mid-rewrite) falls back to the cache and retries on the
		 * next pass. */
		rss_stream_info_t si;
		bool si_ok = (rss_ring_get_stream_info(rctx->ring, &si) == 0);
		if (!si_ok) {
			si.codec = rctx->last_codec;
			si.width = rctx->last_width;
			si.height = rctx->last_height;
			si.fps_num = rctx->last_fps_num;
			si.fps_den = rctx->last_fps_den;
			si.profile = rctx->last_profile;
			si.level = rctx->last_level;
		}
		uint32_t fps = (si.fps_num > 0 && si.fps_den > 0) ? si.fps_num / si.fps_den : 30;
		uint32_t frame_dur = 90000 / fps;

		/*
		 * The session thread builds the SDP from the cache below, and a
		 * reconfigure reaches the header in place -- rvd reuses the ring
		 * across an encoder restart and rewrites its stream info
		 * (rvd_pipeline.c), so nothing is closed, nothing is reopened,
		 * and the reconnect path above never runs. Refresh here, where
		 * the header is already in hand for the pacing above.
		 *
		 * Rate is a cache update and nothing more: a=framerate is
		 * advisory and every client keeps playing. Codec and geometry
		 * are not -- they are what the SPS in the SDP says, so the
		 * clients holding the old answer have to go, and the cached
		 * parameter sets with them: dropping the lengths makes the next
		 * keyframe re-cache (see the sps_len test in the read loop),
		 * which is what a re-DESCRIBE will then be answered with.
		 */
		if (si.fps_num != rctx->last_fps_num || si.fps_den != rctx->last_fps_den) {
			RSS_INFO("ring[%d]: rate now %u/%u fps", rctx->idx, si.fps_num, si.fps_den);
			rctx->last_fps_num = si.fps_num;
			rctx->last_fps_den = si.fps_den;
		}

		if (si.codec != rctx->last_codec || si.width != rctx->last_width ||
		    si.height != rctx->last_height) {
			bool codec_changed = (si.codec != rctx->last_codec);

			RSS_INFO("ring[%d]: stream is now %s %ux%u (was %s %ux%u)", rctx->idx,
				 rsd_codec_name(si.codec), si.width, si.height,
				 rsd_codec_name(rctx->last_codec), (unsigned)rctx->last_width,
				 (unsigned)rctx->last_height);

			rctx->last_codec = si.codec;
			rctx->last_width = si.width;
			rctx->last_height = si.height;
			rctx->last_profile = si.profile;
			rctx->last_level = si.level;

			atomic_store_explicit(&rctx->vps_len, 0, memory_order_relaxed);
			atomic_store_explicit(&rctx->sps_len, 0, memory_order_relaxed);
			atomic_store_explicit(&rctx->pps_len, 0, memory_order_relaxed);

			rsd_drop_stream_clients(srv, stream_idx,
						codec_changed ? "codec" : "resolution");
		}

		for (int burst = 0; burst < 8; burst++) {
			uint32_t length;
			rss_ring_slot_t meta;
			const uint8_t *frame_data;
			uint64_t read_seq = rctx->read_seq;
			uint64_t pre_seq = read_seq;

			ret = rss_ring_read(rctx->ring, &read_seq, rctx->frame_buf,
					    rctx->frame_buf_size, &length, &meta);
			frame_data = rctx->frame_buf;
			if (ret == -ENOSPC) {
				/* The frame outgrew the buffer sized at ring open —
				 * an encoder restart (set-resolution) can raise the
				 * per-buffer stride mid-life. The read reported the
				 * needed size and already advanced past the frame;
				 * grow and continue so it stays a one-frame hiccup,
				 * not a permanent skip storm. */
				uint8_t *bigger = realloc(rctx->frame_buf, length);
				if (bigger) {
					RSS_WARN("video[%d]: frame buffer %u -> %u after "
						 "producer restart",
						 stream_idx, rctx->frame_buf_size, length);
					rctx->frame_buf = bigger;
					rctx->frame_buf_size = length;
				}
				rctx->read_seq = read_seq;
				continue;
			}
			if (ret == RSS_EOVERFLOW) {
				uint64_t skipped = read_seq - pre_seq;
				if (skipped == 0) {
					/* Zero-progress overflow = the producer
					 * recreated the ring (new incarnation, e.g.
					 * rvd re-exec after a raw capture). Reopen
					 * instead of spinning on the stale mapping. */
					RSS_WARN("video[%d] ring incarnation changed, "
						 "reopening (%s)",
						 stream_idx, rctx->ring_name);
					rss_ring_release(rctx->ring);
					rss_ring_close(rctx->ring);
					rctx->ring = NULL;
					break;
				}
				total_overflow += skipped;
				RSS_WARN("video[%d] EOVERFLOW: seq %llu -> %llu (skipped %llu)",
					 stream_idx, (unsigned long long)pre_seq,
					 (unsigned long long)read_seq, (unsigned long long)skipped);
				rctx->read_seq = read_seq;
				rsd_maybe_request_idr(rctx->ring, &last_idr_req_us);
				break;
			}
			if (ret != 0)
				break;

			rctx->read_seq = read_seq;
			total_read++;

			if (video_ts_epoch == 0)
				video_ts_epoch = meta.timestamp;
			uint32_t rtp_ts = (uint32_t)((uint64_t)(meta.timestamp - video_ts_epoch) *
						     90000 / 1000000);

			if (has_last_rtp_ts && (int32_t)(rtp_ts - last_rtp_ts) <= 0)
				rtp_ts = last_rtp_ts + frame_dur;
			last_rtp_ts = rtp_ts;
			has_last_rtp_ts = true;

			if (meta.is_key && rctx->last_codec != 2 && rctx->last_codec != 3 &&
			    atomic_load_explicit(&rctx->sps_len, memory_order_relaxed) == 0)
				rsd_cache_params(rctx, frame_data, length);

			/* Build the per-frame timecode SEI once; every client's
			 * sendq copy gets it spliced before the first VCL NAL. */
			uint8_t sei[RSS_SEI_TS_MAX];
			uint32_t sei_len = 0;
			bool is_h265 = (rctx->last_codec == 1);
			if (srv->sei_timecode && (rctx->last_codec == 0 || rctx->last_codec == 1)) {
				int64_t utc_off;
				uint8_t utc_st;
				if (rss_ring_get_utc(rctx->ring, &utc_off, &utc_st) == 0) {
					int n = rss_sei_build_timestamp(
						sei, sizeof(sei), (int)rctx->last_codec,
						RSS_SEI_PREFIX_ANNEXB,
						(uint64_t)(meta.timestamp + utc_off), utc_st);
					if (n > 0)
						sei_len = (uint32_t)n;
				}
			}

			pthread_mutex_lock(&srv->clients_lock);
			for (int i = 0; i < srv->client_count; i++) {
				rsd_client_t *c = srv->clients[i];
				if (!c || !c->video.playing)
					continue;
				if (c->stream_idx != stream_idx)
					continue;

				/* Hold new/recovering clients until a keyframe.
				 * Orphan P-frames reference pictures the client
				 * never got: live decoders conceal them, but a
				 * recorder (Frigate, ffmpeg -c copy) writes them
				 * into a file that then decodes with missing
				 * refs on every playback. JPEG clients have no
				 * inter refs and never wait. */
				if (c->waiting_keyframe) {
					if (!meta.is_key)
						continue;
					c->waiting_keyframe = false;
					RSS_DEBUG("client[%d] got keyframe", stream_idx);
				}
				/* Timestamp base = first frame actually sent */
				if (!c->video_ts_base_set) {
					c->video_ts_offset = rtp_ts;
					c->video_ts_base_set = true;
				}

				uint32_t client_ts = rtp_ts - c->video_ts_offset + c->video_ts_rand;
				if (c->has_last_video_client_ts &&
				    (int32_t)(client_ts - c->last_video_client_ts) <= 0)
					client_ts = c->last_video_client_ts + frame_dur;
				c->last_video_client_ts = client_ts;
				c->has_last_video_client_ts = true;

				int qret;
				qret = rsd_sendq_push_video(&c->sendq, frame_data, length,
							    client_ts, sei, sei_len, is_h265);
				if (qret == RSD_SENDQ_OK)
					total_pushed++;
				else if (qret == RSD_SENDQ_DROPPED) {
					c->waiting_keyframe = (c->video.jpeg == NULL);
					rsd_maybe_request_idr(rctx->ring, &last_idr_req_us);
				}
			}
			pthread_mutex_unlock(&srv->clients_lock);

			int64_t now_count = rss_timestamp_us();
			if (now_count - last_count_log >= 10000000) {
				RSS_TRACE("video[%d] stats: read=%llu pushed=%llu overflow=%llu",
					  stream_idx, (unsigned long long)total_read,
					  (unsigned long long)total_pushed,
					  (unsigned long long)total_overflow);
				last_count_log = now_count;
			}
		}
	}

	RSS_DEBUG("video reader[%d] exiting", stream_idx);
	return NULL;
}

/* ── JPEG send path (RFC 2435) ── */

static void rsd_send_jpeg_frame(rsd_client_t *c, const uint8_t *data, uint32_t len, uint32_t rtp_ts)
{
	if (!c->video.jpeg || !c->video.playing)
		return;

	pthread_mutex_lock(&c->write_lock);
	(void)!Compy_JpegTransport_send_frame(c->video.jpeg, Compy_RtpTimestamp_Raw(rtp_ts),
					      U8Slice99_new((uint8_t *)data, len));
	pthread_mutex_unlock(&c->write_lock);

	if (c->srv->rtcp_sr) {
		int64_t now = rss_timestamp_us();
		if (c->video.rtcp && now - c->video.last_rtcp > RSD_SR_INTERVAL_US) {
			pthread_mutex_lock(&c->write_lock);
			(void)!Compy_Rtcp_send_sr(c->video.rtcp);
			pthread_mutex_unlock(&c->write_lock);
			c->video.last_rtcp = now;
		}
	}
}

/* ── Audio ring reader thread ── */

static void rsd_send_audio_frame(rsd_client_t *c, uint32_t codec, const uint8_t *data, uint32_t len,
				 uint32_t rtp_ts, int64_t capture_us)
{
	if (!c->audio.rtp || !c->audio.playing)
		return;

	bool marker = false;
	U8Slice99 payload_hdr = U8Slice99_empty();
	uint8_t au_header[4];

	if (codec == RSD_CODEC_AAC) {
		/* RFC 3640 AAC-hbr: AU header section
		 * 2 bytes AU-headers-length (16 = one 16-bit AU header)
		 * 2 bytes AU header: 13-bit AU-size | 3-bit AU-index (0) */
		if (len > 8191) {
			RSS_WARN("AAC frame %u bytes exceeds 13-bit AU-size, dropping", len);
			return;
		}
		au_header[0] = 0x00;
		au_header[1] = 0x10; /* 16 bits of AU header */
		au_header[2] = (uint8_t)((len >> 5) & 0xFF);
		au_header[3] = (uint8_t)((len << 3) & 0xFF);
		payload_hdr = U8Slice99_new(au_header, 4);
		marker = true;
	} else if (codec == RSD_CODEC_OPUS) {
		/* RFC 7587: raw Opus packet, marker on first packet of talkspurt */
		marker = true;
	}

	(void)!Compy_RtpTransport_send_packet(c->audio.rtp, Compy_RtpTimestamp_Raw(rtp_ts), marker,
					      payload_hdr, U8Slice99_new((uint8_t *)data, len));

	/* Media-clock reference for sender reports (RFC 3550 6.4.1): pair
	 * this frame's wire timestamp with its ring capture instant, so the
	 * SR maps the timeline the receiver actually gets instead of
	 * sampling send scheduling -- AAC frames become available on the
	 * 20ms chunk grid and a send-time anchor weaves a frame's worth. */
	Compy_RtpTransport_set_clock_reference(c->audio.rtp, rtp_ts, (uint64_t)capture_us);

	if (c->srv->rtcp_sr) {
		int64_t now = rss_timestamp_us();
		if (c->audio.rtcp && now - c->audio.last_rtcp > RSD_SR_INTERVAL_US) {
			(void)!Compy_Rtcp_send_sr(c->audio.rtcp);
			c->audio.last_rtcp = now;
		}
	}
}

void *rsd_audio_reader_thread(void *arg)
{
	rsd_server_t *srv = arg;

	RSS_DEBUG("audio reader thread started");

	uint32_t audio_codec = 0, rtp_clock = 0;
	uint32_t ring_frame_samples = 0;
	uint8_t audio_buf[4096];
	int64_t audio_ts_epoch = 0;
	uint32_t last_audio_rtp_ts = 0;
	bool has_last_audio_rtp_ts = false;
	int32_t audio_err_ewma = 0;
	uint64_t last_write_seq = 0;
	int idle_count = 0;
	int64_t last_drop_report = rss_timestamp_us();

	/* Initialize codec from pre-opened ring (server opens it before
	 * spawning this thread).  Without this, audio_codec stays 0 and
	 * codec-specific framing (e.g. AAC AU headers) is never applied. */
	if (srv->ring_audio) {
		const rss_ring_header_t *ahdr = rss_ring_get_header(srv->ring_audio);
		audio_codec = ahdr->codec;
		uint32_t audio_clock = ahdr->fps_num;
		rtp_clock = (audio_codec == RSD_CODEC_OPUS) ? 48000 : audio_clock;
		ring_frame_samples = ahdr->width; /* producer-declared samples/frame */
		srv->audio_read_seq = ahdr->write_seq;
		atomic_store(&srv->audio_sdp_codec, audio_codec);
		atomic_store(&srv->audio_sdp_clock, audio_clock);
		atomic_store(&srv->audio_sdp_aot, ahdr->profile);
		RSS_DEBUG("audio codec=%u clock=%u rtp_clock=%u frame_samples=%u", audio_codec,
			  audio_clock, rtp_clock, ring_frame_samples);
	}

	while (rss_running(srv->running)) {
		if (!srv->ring_audio) {
			/* Ring lost — wait for RAD to recreate it */
			srv->ring_audio = rss_ring_open("audio");
			if (!srv->ring_audio) {
				usleep(200000);
				continue;
			}
			rss_ring_check_version(srv->ring_audio, "audio");
			const rss_ring_header_t *ahdr = rss_ring_get_header(srv->ring_audio);
			audio_codec = ahdr->codec;
			uint32_t audio_clock = ahdr->fps_num;
			rtp_clock = (audio_codec == RSD_CODEC_OPUS) ? 48000 : audio_clock;
			ring_frame_samples = ahdr->width;
			srv->audio_read_seq = ahdr->write_seq;
			atomic_store(&srv->audio_sdp_codec, audio_codec);
			atomic_store(&srv->audio_sdp_clock, audio_clock);
			atomic_store(&srv->audio_sdp_aot, ahdr->profile);
			audio_ts_epoch = 0;
			last_audio_rtp_ts = 0;
			has_last_audio_rtp_ts = false;
			last_write_seq = 0;
			idle_count = 0;

			/* Ring reconnected — reset client audio bases */
			pthread_mutex_lock(&srv->clients_lock);
			for (int i = 0; i < srv->client_count; i++) {
				rsd_client_t *c = srv->clients[i];
				if (c && c->audio.playing)
					c->audio_ts_base_set = false;
			}
			pthread_mutex_unlock(&srv->clients_lock);

			RSS_DEBUG("audio codec=%u clock=%u rtp_clock=%u", audio_codec, audio_clock,
				  rtp_clock);
		}

		int ret = rss_ring_wait(srv->ring_audio, 100);
		if (ret != 0) {
			const rss_ring_header_t *h = rss_ring_get_header(srv->ring_audio);
			uint64_t ws = h->write_seq;
			if (ws == last_write_seq)
				idle_count++;
			else
				idle_count = 0;
			last_write_seq = ws;

			if (idle_count >= 20) {
				RSS_DEBUG("audio ring idle, closing");
				rss_ring_close(srv->ring_audio);
				srv->ring_audio = NULL;
				idle_count = 0;
			}
			continue;
		}
		idle_count = 0;

		for (int burst = 0; burst < 16; burst++) {
			uint32_t length;
			rss_ring_slot_t meta;
			uint64_t read_seq = srv->audio_read_seq;

			uint64_t audio_pre_seq = read_seq;
			ret = rss_ring_read(srv->ring_audio, &read_seq, audio_buf,
					    sizeof(audio_buf), &length, &meta);
			if (ret == RSS_EOVERFLOW) {
				if (read_seq == audio_pre_seq) {
					/* Producer recreated the ring: reopen. */
					RSS_WARN("audio ring incarnation changed, reopening");
					rss_ring_close(srv->ring_audio);
					srv->ring_audio = NULL;
					break;
				}
				srv->audio_read_seq = read_seq;
				break;
			}
			if (ret != 0)
				break;

			srv->audio_read_seq = read_seq;

			uint32_t frame_samples;
			switch (audio_codec) {
			case RSD_CODEC_AAC:
				/* HE-AAC frames carry 2048 samples; the producer
				 * declares the size in the ring header. */
				frame_samples = ring_frame_samples ? ring_frame_samples : 1024;
				break;
			case RSD_CODEC_OPUS:
				frame_samples = 960;
				break;
			case RSD_CODEC_L16:
				frame_samples = length / 2;
				break;
			default:
				frame_samples = length;
				break;
			}

			/* Audio RTP timestamps: smooth frame cadence steered
			 * toward the ring capture time (rad's wall-slewed
			 * clock). A free-running counter drifts with the
			 * source clock and silently absorbs ring-overflow
			 * gaps into a permanent A/V offset; raw ring times
			 * carry per-frame arrival quantization that clients
			 * read as an unstable rate. Steering keeps exact
			 * spacing on the wire while the ring clock governs
			 * the long-run rate: snap forward on real gaps (>4
			 * frames, clients render a gap), re-anchor the
			 * mapping if ring time regresses (rad restart --
			 * never send backward RTP time), and slew
			 * proportionally on a filtered error. The filter
			 * absorbs the arrival quantization that once forced
			 * a deadband a full frame wide -- and that deadband
			 * let the wire wander a frame off the ring clock and
			 * walk back in 1ms steps, a sawtooth every RTCP
			 * sender report faithfully republished as ~15ms
			 * timeline corrections (measured on a T31 at 8kHz). */
			if (audio_ts_epoch == 0)
				audio_ts_epoch = (int64_t)meta.timestamp;
			uint32_t ring_rtp = (uint32_t)((uint64_t)(meta.timestamp - audio_ts_epoch) *
						       rtp_clock / 1000000);
			uint32_t rtp_ts;
			if (!has_last_audio_rtp_ts) {
				rtp_ts = ring_rtp;
			} else {
				rtp_ts = last_audio_rtp_ts + frame_samples;
				int32_t err = (int32_t)(ring_rtp - rtp_ts);
				if (rtp_clock == 0) {
					/* degenerate ring header: plain cadence */
				} else if (err > (int32_t)frame_samples * 4) {
					rtp_ts = ring_rtp;
					audio_err_ewma = 0;
				} else if (err < -(int32_t)frame_samples * 4) {
					audio_ts_epoch = (int64_t)meta.timestamp -
							 (int64_t)rtp_ts * 1000000 / rtp_clock;
					audio_err_ewma = 0;
				} else {
					audio_err_ewma += (err - audio_err_ewma) / 16;
					int32_t slew = audio_err_ewma / 16;
					int32_t max_slew = (int32_t)(rtp_clock / 1000); /* 1ms */
					if (slew > max_slew)
						slew = max_slew;
					else if (slew < -max_slew)
						slew = -max_slew;
					rtp_ts += slew;
				}
			}
			last_audio_rtp_ts = rtp_ts;
			has_last_audio_rtp_ts = true;

			pthread_mutex_lock(&srv->clients_lock);
			for (int i = 0; i < srv->client_count; i++) {
				rsd_client_t *c = srv->clients[i];
				if (!c || !c->audio.playing)
					continue;

				/* Gate audio on first video frame (not keyframe).
				 * Both timelines anchor to the same instant. */
				if ((c->video.nal || c->video.jpeg) && c->video.playing &&
				    !c->video_ts_base_set)
					continue;

				if (!c->audio_ts_base_set) {
					c->audio_ts_offset = rtp_ts;
					c->audio_ts_base_set = true;
				}
				uint32_t client_ts = rtp_ts - c->audio_ts_offset + c->audio_ts_rand;
				if (c->has_last_audio_client_ts &&
				    (int32_t)(client_ts - c->last_audio_client_ts) <= 0)
					client_ts = c->last_audio_client_ts + frame_samples;
				c->last_audio_client_ts = client_ts;
				c->has_last_audio_client_ts = true;
				rsd_sendq_push_audio(&c->sendq, audio_codec, audio_buf, length,
						     client_ts, (int64_t)meta.timestamp);
			}
			pthread_mutex_unlock(&srv->clients_lock);
		}

		/*
		 * Report send-queue discards. A client that cannot keep up loses
		 * audio here, downstream of capture, and until this was counted
		 * the symptom was indistinguishable at the log from the SDK
		 * losing periods -- which is what sent the first round of
		 * dropout debugging to the wrong half of the pipeline. Reported
		 * only when non-zero, so a healthy stream stays silent.
		 */
		int64_t drop_now = rss_timestamp_us();
		if (drop_now - last_drop_report >= 30000000) {
			last_drop_report = drop_now;
			pthread_mutex_lock(&srv->clients_lock);
			for (int i = 0; i < srv->client_count; i++) {
				rsd_client_t *c = srv->clients[i];
				if (!c)
					continue;
				pthread_mutex_lock(&c->sendq.lock);
				uint32_t ov = c->sendq.overflows;
				uint32_t da = c->sendq.drop_audio;
				uint32_t dv = c->sendq.drop_video;
				c->sendq.overflows = 0;
				c->sendq.drop_audio = 0;
				c->sendq.drop_video = 0;
				pthread_mutex_unlock(&c->sendq.lock);
				if (ov)
					RSS_WARN("client %d sendq overflowed %u time(s): dropped "
						 "%u audio, %u video -- the client is not draining "
						 "fast enough",
						 i, ov, da, dv);
			}
			pthread_mutex_unlock(&srv->clients_lock);
		}
	}

	RSS_DEBUG("audio reader thread exiting");
	return NULL;
}

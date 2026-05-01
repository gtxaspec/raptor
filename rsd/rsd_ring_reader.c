/*
 * rsd_ring_reader.c -- SHM ring consumer + frame distribution
 *
 * A dedicated thread reads frames from the SHM ring and distributes
 * them to all playing RTSP clients via their compy NalTransport.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include <pthread.h>

#include "rsd.h"

/* Forward declarations — called by send thread, defined below */
static void rsd_send_audio_frame(rsd_client_t *c, uint32_t codec, const uint8_t *data, uint32_t len,
				 uint32_t rtp_ts);
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
static void rsd_cache_sps_pps(rsd_ring_ctx_t *rctx, const uint8_t *data, uint32_t len)
{
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
		if (nalu_len < 1) {
			p = nalu_end;
			continue;
		}

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

		p = nalu_end;
	}
}

/* ── Per-client send queue ── */

int rsd_sendq_init(rsd_sendq_t *q)
{
	memset(q, 0, sizeof(*q));
	if (pthread_mutex_init(&q->lock, NULL) != 0)
		return -1;
	if (pthread_cond_init(&q->cond, NULL) != 0) {
		pthread_mutex_destroy(&q->lock);
		return -1;
	}
	return 0;
}

static void sendq_release_entry(rsd_sendq_entry_t *e)
{
	if (!e->zerocopy)
		free((void *)e->data);
	e->data = NULL;
}

void rsd_sendq_destroy(rsd_sendq_t *q)
{
	while (q->count > 0) {
		sendq_release_entry(&q->entries[q->tail]);
		q->tail = (q->tail + 1) % RSD_SENDQ_SLOTS;
		q->count--;
	}
	pthread_cond_destroy(&q->cond);
	pthread_mutex_destroy(&q->lock);
}

static void sendq_flush_locked(rsd_sendq_t *q)
{
	while (q->count > 0) {
		sendq_release_entry(&q->entries[q->tail]);
		q->tail = (q->tail + 1) % RSD_SENDQ_SLOTS;
		q->count--;
	}
	q->head = 0;
	q->tail = 0;
}

/*
 * Push a video frame onto the client's sendq. The data is copied so
 * the reader can immediately overwrite frame_buf with the next ring
 * frame — no barrier wait on the send thread. The old zero-copy
 * barrier capped throughput at 1 / send_latency on single-core SoCs
 * and was the root of the residual IDR clustering we saw even after
 * the crypto-path optimizations.
 */
static int rsd_sendq_push_video(rsd_sendq_t *q, const uint8_t *data, uint32_t len, uint32_t rtp_ts)
{
	uint8_t *copy = malloc(len);
	if (!copy)
		return -1;
	memcpy(copy, data, len);

	pthread_mutex_lock(&q->lock);
	if (q->shutdown) {
		pthread_mutex_unlock(&q->lock);
		free(copy);
		return -1;
	}

	bool dropped = false;
	if (q->count >= RSD_SENDQ_SLOTS) {
		sendq_flush_locked(q);
		dropped = true;
	}

	rsd_sendq_entry_t *slot = &q->entries[q->head];
	slot->data = copy;
	slot->len = len;
	slot->rtp_ts = rtp_ts;
	slot->type = RSD_FRAME_VIDEO;
	slot->codec = 0;

	q->head = (q->head + 1) % RSD_SENDQ_SLOTS;
	q->count++;

	pthread_cond_signal(&q->cond);
	pthread_mutex_unlock(&q->lock);
	return dropped ? RSD_SENDQ_DROPPED : RSD_SENDQ_OK;
}

static int rsd_sendq_push_zerocopy(rsd_sendq_t *q, const uint8_t *data, uint32_t len,
				   uint32_t rtp_ts, uint8_t buf_idx, uint32_t buf_gen)
{
	pthread_mutex_lock(&q->lock);
	if (q->shutdown) {
		pthread_mutex_unlock(&q->lock);
		return -1;
	}

	bool dropped = false;
	if (q->count >= RSD_SENDQ_SLOTS) {
		sendq_flush_locked(q);
		dropped = true;
	}

	rsd_sendq_entry_t *slot = &q->entries[q->head];
	slot->data = data;
	slot->len = len;
	slot->rtp_ts = rtp_ts;
	slot->type = RSD_FRAME_VIDEO;
	slot->codec = 0;
	slot->zerocopy = true;
	slot->buf_idx = buf_idx;
	slot->buf_gen = buf_gen;

	q->head = (q->head + 1) % RSD_SENDQ_SLOTS;
	q->count++;

	pthread_cond_signal(&q->cond);
	pthread_mutex_unlock(&q->lock);
	return dropped ? RSD_SENDQ_DROPPED : RSD_SENDQ_OK;
}

static int rsd_sendq_push_audio(rsd_sendq_t *q, uint32_t codec, const uint8_t *data, uint32_t len,
				uint32_t rtp_ts)
{
	uint8_t *copy = malloc(len);
	if (!copy)
		return -1;
	memcpy(copy, data, len);

	pthread_mutex_lock(&q->lock);
	if (q->shutdown) {
		pthread_mutex_unlock(&q->lock);
		free(copy);
		return -1;
	}

	if (q->count >= RSD_SENDQ_SLOTS) {
		sendq_flush_locked(q);
	}

	rsd_sendq_entry_t *slot = &q->entries[q->head];
	slot->data = copy;
	slot->len = len;
	slot->rtp_ts = rtp_ts;
	slot->type = RSD_FRAME_AUDIO;
	slot->codec = codec;
	slot->zerocopy = false;

	q->head = (q->head + 1) % RSD_SENDQ_SLOTS;
	q->count++;

	pthread_cond_signal(&q->cond);
	pthread_mutex_unlock(&q->lock);
	return RSD_SENDQ_OK;
}

/* Try to pop and send one audio entry from the sendq.
 * Called between video NALUs to interleave audio with large IDR frames. */
static void sendq_drain_audio(rsd_client_t *c)
{
	rsd_sendq_t *q = &c->sendq;
	rsd_sendq_entry_t audio;
	bool got = false;

	pthread_mutex_lock(&q->lock);
	/* Scan for the first audio entry (may not be at the tail) */
	for (int i = 0; i < q->count; i++) {
		int idx = (q->tail + i) % RSD_SENDQ_SLOTS;
		if (q->entries[idx].type == RSD_FRAME_AUDIO) {
			audio = q->entries[idx];
			q->entries[idx].data = NULL;
			/* Shift remaining entries to fill the gap */
			for (int j = i; j > 0; j--) {
				int dst = (q->tail + j) % RSD_SENDQ_SLOTS;
				int src = (q->tail + j - 1) % RSD_SENDQ_SLOTS;
				q->entries[dst] = q->entries[src];
			}
			q->entries[q->tail].data = NULL;
			q->tail = (q->tail + 1) % RSD_SENDQ_SLOTS;
			q->count--;
			got = true;
			break;
		}
	}
	pthread_mutex_unlock(&q->lock);

	if (got) {
		pthread_mutex_lock(&c->write_lock);
		rsd_send_audio_frame(c, audio.codec, audio.data, audio.len, audio.rtp_ts);
		pthread_mutex_unlock(&c->write_lock);
		sendq_release_entry(&audio);
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
		if (c->video.rtcp && now - c->video.last_rtcp > 30000000) {
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

		/* Zero-copy: validate encoder buffer wasn't recycled. */
		if (entry.zerocopy && c->srv) {
			rss_ring_t *ring = c->srv->video[c->stream_idx].ring;
			if (!ring) {
				sendq_release_entry(&entry);
				continue;
			}
			const rss_ring_header_t *hdr = rss_ring_get_header(ring);
			if (hdr && entry.buf_idx < RSS_RING_MAX_REF_BUFS) {
				uint32_t cur = atomic_load_explicit(
					&hdr->ref_buf_gen[entry.buf_idx], memory_order_acquire);
				if (cur != entry.buf_gen) {
					sendq_release_entry(&entry);
					continue;
				}
			}
		}

		if (entry.type == RSD_FRAME_VIDEO) {
			if (c->video.jpeg)
				rsd_send_jpeg_frame(c, entry.data, entry.len, entry.rtp_ts);
			else
				rsd_send_video_interleaved(c, entry.data, entry.len, entry.rtp_ts);
		} else {
			pthread_mutex_lock(&c->write_lock);
			rsd_send_audio_frame(c, entry.codec, entry.data, entry.len, entry.rtp_ts);
			pthread_mutex_unlock(&c->write_lock);
		}

		/* Post-send zerocopy validation */
		if (entry.zerocopy && c->srv) {
			rss_ring_t *ring = c->srv->video[c->stream_idx].ring;
			if (ring) {
				const rss_ring_header_t *hdr = rss_ring_get_header(ring);
				if (hdr && entry.buf_idx < RSS_RING_MAX_REF_BUFS) {
					uint32_t cur = atomic_load_explicit(
						&hdr->ref_buf_gen[entry.buf_idx],
						memory_order_acquire);
					if (cur != entry.buf_gen)
						RSS_WARN("zerocopy buf %u recycled during send",
							 entry.buf_idx);
				}
			}
		}

		sendq_release_entry(&entry);
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
					rss_ring_close(rctx->ring);
					rctx->ring = NULL;
					rctx->frame_buf_size = 0;
					continue;
				}
				free(rctx->frame_buf);
				rctx->frame_buf = new_buf;
				rctx->frame_buf_size = max_frame;
			}
			rctx->read_seq = 0;
			last_write_seq = 0;
			idle_count = 0;
			video_ts_epoch = 0;
			last_rtp_ts = 0;
			has_last_rtp_ts = false;
			atomic_store_explicit(&rctx->sps_len, 0, memory_order_relaxed);
			atomic_store_explicit(&rctx->pps_len, 0, memory_order_relaxed);

			/* Cache all SDP-relevant fields so the session thread
			 * never needs to dereference the ring pointer. */
			const rss_ring_header_t *new_hdr = rss_ring_get_header(rctx->ring);
			bool codec_changed = (new_hdr->codec != rctx->last_codec);
			rctx->last_codec = new_hdr->codec;
			rctx->last_width = new_hdr->width;
			rctx->last_height = new_hdr->height;
			rctx->last_fps_num = new_hdr->fps_num;
			rctx->last_fps_den = new_hdr->fps_den;
			rctx->last_profile = new_hdr->profile;
			rctx->last_level = new_hdr->level;

			/* Ring reconnected — reset all clients on this stream
			 * so they re-sync from the next keyframe. If codec changed,
			 * disconnect existing clients (RTSP can't renegotiate SDP
			 * mid-session — clients must reconnect for new codec). */
			pthread_mutex_lock(&srv->clients_lock);
			for (int i = 0; i < srv->client_count; i++) {
				rsd_client_t *c = srv->clients[i];
				if (c && c->stream_idx == stream_idx && c->video.playing) {
					if (codec_changed) {
						shutdown(c->fd, SHUT_RDWR);
						RSS_INFO("disconnecting client on stream %d (codec "
							 "changed)",
							 stream_idx);
					} else {
						c->waiting_keyframe = true;
						c->video_ts_base_set = false;
						c->audio_ts_base_set = false;
					}
				}
			}
			pthread_mutex_unlock(&srv->clients_lock);

			RSS_INFO("video reader[%d] reconnected (%s%s)", stream_idx, rctx->ring_name,
				 codec_changed ? ", codec changed" : "");
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

			if (idle_count >= 20) {
				RSS_DEBUG("video reader[%d] idle, closing ring (%s)", stream_idx,
					  rctx->ring_name);
				rss_ring_close(rctx->ring);
				rctx->ring = NULL;
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
		const rss_ring_header_t *rhdr = rss_ring_get_header(rctx->ring);
		bool use_zerocopy = (rhdr->flags & RSS_RING_FLAG_REFMODE);
		uint32_t fps = (rhdr->fps_num > 0 && rhdr->fps_den > 0)
				       ? rhdr->fps_num / rhdr->fps_den
				       : 30;
		uint32_t frame_dur = 90000 / fps;

		for (int burst = 0; burst < 8; burst++) {
			uint32_t length;
			rss_ring_slot_t meta;
			const uint8_t *frame_data;
			uint64_t read_seq = rctx->read_seq;
			uint64_t pre_seq = read_seq;

			if (use_zerocopy) {
				ret = rss_ring_peek(rctx->ring, &read_seq, &frame_data, &length,
						    &meta);
			} else {
				ret = rss_ring_read(rctx->ring, &read_seq, rctx->frame_buf,
						    rctx->frame_buf_size, &length, &meta);
				frame_data = rctx->frame_buf;
			}
			if (ret == RSS_EOVERFLOW) {
				uint64_t skipped = read_seq - pre_seq;
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
				rsd_cache_sps_pps(rctx, frame_data, length);

			pthread_mutex_lock(&srv->clients_lock);
			for (int i = 0; i < srv->client_count; i++) {
				rsd_client_t *c = srv->clients[i];
				if (!c || !c->video.playing)
					continue;
				if (c->stream_idx != stream_idx)
					continue;

				/* Set timestamp base on first frame (any type).
				 * Pre-IDR P-frames establish the PTS chain even
				 * though the decoder can't display them. */
				if (!c->video_ts_base_set) {
					c->video_ts_offset = rtp_ts;
					c->video_ts_base_set = true;
				}
				if (c->waiting_keyframe && meta.is_key) {
					c->waiting_keyframe = false;
					RSS_DEBUG("client[%d] got keyframe", stream_idx);
				}

				uint32_t client_ts = rtp_ts - c->video_ts_offset + c->video_ts_rand;
				if (c->has_last_video_client_ts &&
				    (int32_t)(client_ts - c->last_video_client_ts) <= 0)
					client_ts = c->last_video_client_ts + frame_dur;
				c->last_video_client_ts = client_ts;
				c->has_last_video_client_ts = true;

				int qret;
				if (use_zerocopy) {
					qret = rsd_sendq_push_zerocopy(&c->sendq, frame_data,
								       length, client_ts,
								       meta.buf_idx, meta.buf_gen);
				} else {
					qret = rsd_sendq_push_video(&c->sendq, frame_data, length,
								    client_ts);
				}
				if (qret == RSD_SENDQ_OK)
					total_pushed++;
				else if (qret == RSD_SENDQ_DROPPED) {
					c->waiting_keyframe = (c->video.jpeg == NULL);
					c->audio_ts_base_set = false;
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
		if (c->video.rtcp && now - c->video.last_rtcp > 30000000) {
			pthread_mutex_lock(&c->write_lock);
			(void)!Compy_Rtcp_send_sr(c->video.rtcp);
			pthread_mutex_unlock(&c->write_lock);
			c->video.last_rtcp = now;
		}
	}
}

/* ── Audio ring reader thread ── */

static void rsd_send_audio_frame(rsd_client_t *c, uint32_t codec, const uint8_t *data, uint32_t len,
				 uint32_t rtp_ts)
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

	if (c->srv->rtcp_sr) {
		int64_t now = rss_timestamp_us();
		if (c->audio.rtcp && now - c->audio.last_rtcp > 30000000) {
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
	uint8_t audio_buf[4096];
	int64_t audio_ts_epoch = 0;
	uint32_t last_audio_rtp_ts = 0;
	bool has_last_audio_rtp_ts = false;
	uint64_t last_write_seq = 0;
	int idle_count = 0;

	/* Initialize codec from pre-opened ring (server opens it before
	 * spawning this thread).  Without this, audio_codec stays 0 and
	 * codec-specific framing (e.g. AAC AU headers) is never applied. */
	if (srv->ring_audio) {
		const rss_ring_header_t *ahdr = rss_ring_get_header(srv->ring_audio);
		audio_codec = ahdr->codec;
		uint32_t audio_clock = ahdr->fps_num;
		rtp_clock = (audio_codec == RSD_CODEC_OPUS) ? 48000 : audio_clock;
		srv->audio_read_seq = ahdr->write_seq;
		RSS_DEBUG("audio codec=%u clock=%u rtp_clock=%u", audio_codec, audio_clock,
			  rtp_clock);
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
			srv->audio_read_seq = ahdr->write_seq;
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

			ret = rss_ring_read(srv->ring_audio, &read_seq, audio_buf,
					    sizeof(audio_buf), &length, &meta);
			if (ret == RSS_EOVERFLOW) {
				srv->audio_read_seq = read_seq;
				break;
			}
			if (ret != 0)
				break;

			srv->audio_read_seq = read_seq;

			/* Synthetic audio timestamps at exact frame cadence,
			 * anchored to CLOCK_MONOTONIC — the unified timebase
			 * shared by video RTP and RTCP NTP. */
			int64_t now_us = rss_timestamp_us();
			if (audio_ts_epoch == 0)
				audio_ts_epoch = now_us;
			uint32_t frame_samples;
			switch (audio_codec) {
			case RSD_CODEC_AAC:
				frame_samples = 1024;
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
			uint32_t rtp_ts;
			if (!has_last_audio_rtp_ts) {
				rtp_ts = (uint32_t)((uint64_t)(now_us - audio_ts_epoch) *
						    rtp_clock / 1000000);
			} else {
				rtp_ts = last_audio_rtp_ts + frame_samples;
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
				rsd_sendq_push_audio(&c->sendq, audio_codec, audio_buf, length,
						     client_ts);
			}
			pthread_mutex_unlock(&srv->clients_lock);
		}
	}

	RSS_DEBUG("audio reader thread exiting");
	return NULL;
}

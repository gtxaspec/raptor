/*
 * rsd555_reader.c -- SHM ring consumer threads
 *
 * Dedicated threads read frames from SHM rings and fan them out
 * to all registered per-client source queues.
 */

#include "rsd555.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

const char *const rsd555_ring_names[RSD555_STREAM_COUNT] = {
	"main", "sub", "s1_main", "s1_sub", "s2_main", "s2_sub",
};

/* ── Source registration (called from live555 thread) ── */

int rsd555_video_add_source(rsd555_video_ctx_t *ctx, rsd555_frame_queue_t *q)
{
	int ret = -1;
	pthread_mutex_lock(&ctx->sources_lock);
	if (ctx->source_count < RSD555_MAX_SOURCES) {
		ctx->sources[ctx->source_count++] = q;
		ret = 0;
	}
	pthread_mutex_unlock(&ctx->sources_lock);
	return ret;
}

void rsd555_video_remove_source(rsd555_video_ctx_t *ctx, rsd555_frame_queue_t *q)
{
	pthread_mutex_lock(&ctx->sources_lock);
	for (int i = 0; i < ctx->source_count; i++) {
		if (ctx->sources[i] == q) {
			ctx->sources[i] = ctx->sources[ctx->source_count - 1];
			ctx->source_count--;
			break;
		}
	}
	pthread_mutex_unlock(&ctx->sources_lock);
}

int rsd555_audio_add_source(rsd555_audio_ctx_t *ctx, rsd555_frame_queue_t *q)
{
	int ret = -1;
	pthread_mutex_lock(&ctx->sources_lock);
	if (ctx->source_count < RSD555_MAX_SOURCES) {
		ctx->sources[ctx->source_count++] = q;
		ret = 0;
	}
	pthread_mutex_unlock(&ctx->sources_lock);
	return ret;
}

void rsd555_audio_remove_source(rsd555_audio_ctx_t *ctx, rsd555_frame_queue_t *q)
{
	pthread_mutex_lock(&ctx->sources_lock);
	for (int i = 0; i < ctx->source_count; i++) {
		if (ctx->sources[i] == q) {
			ctx->sources[i] = ctx->sources[ctx->source_count - 1];
			ctx->source_count--;
			break;
		}
	}
	pthread_mutex_unlock(&ctx->sources_lock);
}

/* ── Fan-out: snapshot sources, push shared frame refs ── */

static void fan_out_shared(pthread_mutex_t *lock,
			   rsd555_frame_queue_t **sources, int *source_count,
			   rsd555_shared_frame_t *sf)
{
	rsd555_frame_queue_t *snap[RSD555_MAX_SOURCES];
	int count;

	pthread_mutex_lock(lock);
	count = *source_count;
	memcpy(snap, sources, count * sizeof(snap[0]));
	pthread_mutex_unlock(lock);

	for (int i = 0; i < count; i++)
		rsd555_queue_push_ref(snap[i], sf);
}

/* ── SPS/PPS/VPS caching (H.264 + H.265) ── */

static void cache_sps_pps_h264(rsd555_video_ctx_t *ctx, const uint8_t *data, uint32_t len)
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
		if (nal_type == 7 && nalu_len <= sizeof(ctx->sps)) {
			memcpy(ctx->sps, nalu_start, nalu_len);
			__atomic_store_n(&ctx->sps_len, (uint16_t)nalu_len, __ATOMIC_RELEASE);
		} else if (nal_type == 8 && nalu_len <= sizeof(ctx->pps)) {
			memcpy(ctx->pps, nalu_start, nalu_len);
			__atomic_store_n(&ctx->pps_len, (uint16_t)nalu_len, __ATOMIC_RELEASE);
		}

		p = nalu_end;
	}
}

static void cache_sps_pps_h265(rsd555_video_ctx_t *ctx, const uint8_t *data, uint32_t len)
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
		if (nalu_len < 2) {
			p = nalu_end;
			continue;
		}

		/* H.265 NAL type is bits 1-6 of the first byte */
		uint8_t nal_type = (nalu_start[0] >> 1) & 0x3F;
		if (nal_type == 32 && nalu_len <= sizeof(ctx->vps)) {
			memcpy(ctx->vps, nalu_start, nalu_len);
			__atomic_store_n(&ctx->vps_len, (uint16_t)nalu_len, __ATOMIC_RELEASE);
		} else if (nal_type == 33 && nalu_len <= sizeof(ctx->sps)) {
			memcpy(ctx->sps, nalu_start, nalu_len);
			__atomic_store_n(&ctx->sps_len, (uint16_t)nalu_len, __ATOMIC_RELEASE);
		} else if (nal_type == 34 && nalu_len <= sizeof(ctx->pps)) {
			memcpy(ctx->pps, nalu_start, nalu_len);
			__atomic_store_n(&ctx->pps_len, (uint16_t)nalu_len, __ATOMIC_RELEASE);
		}

		p = nalu_end;
	}
}

static void maybe_request_idr(rss_ring_t *ring, int64_t *last_req_us)
{
	int64_t now_us = rss_timestamp_us();
	if (now_us - *last_req_us > RSD555_IDR_REQ_MIN_US) {
		rss_ring_request_idr(ring);
		*last_req_us = now_us;
	}
}

/* ── Video reader thread ── */

void *rsd555_video_reader_thread(void *arg)
{
	rsd555_video_ctx_t *ctx = arg;
	uint64_t last_write_seq = 0;
	int idle_count = 0;

	RSS_DEBUG("video reader[%d] started (%s)", ctx->idx, ctx->ring_name);

	while (rss_running(ctx->running)) {
		if (!ctx->ring) {
			ctx->ring = rss_ring_open(ctx->ring_name);
			if (!ctx->ring) {
				usleep(200000);
				continue;
			}
			rss_ring_check_version(ctx->ring, ctx->ring_name);
			uint32_t max_frame = rss_ring_max_frame_size(ctx->ring);
			if (max_frame < 256 * 1024)
				max_frame = 256 * 1024;
			if (ctx->frame_buf_size < max_frame) {
				uint8_t *new_buf = malloc(max_frame);
				if (!new_buf) {
					rss_ring_close(ctx->ring);
					ctx->ring = NULL;
					continue;
				}
				free(ctx->frame_buf);
				ctx->frame_buf = new_buf;
				ctx->frame_buf_size = max_frame;
			}
			const rss_ring_header_t *hdr = rss_ring_get_header(ctx->ring);
			ctx->read_seq = __atomic_load_n(&hdr->write_seq, __ATOMIC_RELAXED);
			last_write_seq = 0;
			idle_count = 0;
			__atomic_store_n(&ctx->vps_len, 0, __ATOMIC_RELAXED);
			__atomic_store_n(&ctx->sps_len, 0, __ATOMIC_RELAXED);
			__atomic_store_n(&ctx->pps_len, 0, __ATOMIC_RELAXED);

			ctx->codec = hdr->codec;
			ctx->width = hdr->width;
			ctx->height = hdr->height;
			ctx->fps_num = hdr->fps_num;
			ctx->fps_den = hdr->fps_den;
			ctx->profile = hdr->profile;
			ctx->level = hdr->level;

			rss_ring_request_idr(ctx->ring);
			RSS_INFO("video reader[%d] connected (%s %ux%u)", ctx->idx,
				 ctx->ring_name, ctx->width, ctx->height);
		}

		int ret = rss_ring_wait(ctx->ring, 100);
		if (ret != 0) {
			const rss_ring_header_t *h = rss_ring_get_header(ctx->ring);
			uint64_t ws = __atomic_load_n(&h->write_seq, __ATOMIC_RELAXED);
			if (ws == last_write_seq)
				idle_count++;
			else
				idle_count = 0;
			last_write_seq = ws;

			if (idle_count >= 20) {
				RSS_DEBUG("video reader[%d] idle, closing ring", ctx->idx);
				rss_ring_close(ctx->ring);
				ctx->ring = NULL;
				idle_count = 0;
			}
			continue;
		}
		idle_count = 0;

		if (!ctx->frame_buf)
			continue;

		for (int burst = 0; burst < 8; burst++) {
			uint32_t length;
			rss_ring_slot_t meta;
			uint64_t read_seq = ctx->read_seq;

			ret = rss_ring_read(ctx->ring, &read_seq, ctx->frame_buf,
					    ctx->frame_buf_size, &length, &meta);
			if (ret == RSS_EOVERFLOW) {
				ctx->read_seq = read_seq;
				maybe_request_idr(ctx->ring, &ctx->last_idr_req_us);
				break;
			}
			if (ret != 0)
				break;

			ctx->read_seq = read_seq;

			if (meta.is_key &&
			    __atomic_load_n(&ctx->sps_len, __ATOMIC_RELAXED) == 0) {
				if (ctx->codec == 1)
					cache_sps_pps_h265(ctx, ctx->frame_buf, length);
				else
					cache_sps_pps_h264(ctx, ctx->frame_buf, length);
			}

			/* Only allocate + fan out when clients are connected.
			 * Shared frame is right-sized to actual length. */
			int sc = __atomic_load_n(&ctx->source_count, __ATOMIC_RELAXED);
			if (sc > 0) {
				rsd555_shared_frame_t *sf = rsd555_shared_frame_new(
					ctx->frame_buf, length, meta.timestamp,
					meta.nal_type, meta.is_key);
				if (sf) {
					fan_out_shared(&ctx->sources_lock, ctx->sources,
						       &ctx->source_count, sf);
					rsd555_shared_frame_unref(sf);
				}
			}
		}
	}

	if (ctx->ring) {
		rss_ring_close(ctx->ring);
		ctx->ring = NULL;
	}
	free(ctx->frame_buf);
	ctx->frame_buf = NULL;

	RSS_DEBUG("video reader[%d] exiting", ctx->idx);
	return NULL;
}

/* ── Audio reader thread ── */

void *rsd555_audio_reader_thread(void *arg)
{
	rsd555_audio_ctx_t *ctx = arg;
	uint8_t audio_buf[4096];
	uint64_t last_write_seq = 0;
	int idle_count = 0;

	RSS_DEBUG("audio reader started");

	while (rss_running(ctx->running)) {
		if (!ctx->ring) {
			ctx->ring = rss_ring_open("audio");
			if (!ctx->ring) {
				usleep(200000);
				continue;
			}
			rss_ring_check_version(ctx->ring, "audio");
			const rss_ring_header_t *hdr = rss_ring_get_header(ctx->ring);
			ctx->codec = hdr->codec;
			ctx->sample_rate = hdr->fps_num;
			ctx->read_seq = __atomic_load_n(&hdr->write_seq, __ATOMIC_RELAXED);
			last_write_seq = 0;
			idle_count = 0;
			RSS_INFO("audio ring connected (codec=%u rate=%u)",
				 ctx->codec, ctx->sample_rate);
		}

		int ret = rss_ring_wait(ctx->ring, 100);
		if (ret != 0) {
			const rss_ring_header_t *h = rss_ring_get_header(ctx->ring);
			uint64_t ws = __atomic_load_n(&h->write_seq, __ATOMIC_RELAXED);
			if (ws == last_write_seq)
				idle_count++;
			else
				idle_count = 0;
			last_write_seq = ws;

			if (idle_count >= 20) {
				RSS_DEBUG("audio ring idle, closing");
				rss_ring_close(ctx->ring);
				ctx->ring = NULL;
				idle_count = 0;
			}
			continue;
		}
		idle_count = 0;

		for (int burst = 0; burst < 16; burst++) {
			uint32_t length;
			rss_ring_slot_t meta;
			uint64_t read_seq = ctx->read_seq;

			ret = rss_ring_read(ctx->ring, &read_seq, audio_buf,
					    sizeof(audio_buf), &length, &meta);
			if (ret == RSS_EOVERFLOW) {
				ctx->read_seq = read_seq;
				break;
			}
			if (ret != 0)
				break;

			ctx->read_seq = read_seq;

			int sc = __atomic_load_n(&ctx->source_count, __ATOMIC_RELAXED);
			if (sc > 0) {
				rsd555_shared_frame_t *sf = rsd555_shared_frame_new(
					audio_buf, length, meta.timestamp,
					meta.nal_type, 0);
				if (sf) {
					fan_out_shared(&ctx->sources_lock, ctx->sources,
						       &ctx->source_count, sf);
					rsd555_shared_frame_unref(sf);
				}
			}
		}
	}

	if (ctx->ring) {
		rss_ring_close(ctx->ring);
		ctx->ring = NULL;
	}

	RSS_DEBUG("audio reader exiting");
	return NULL;
}

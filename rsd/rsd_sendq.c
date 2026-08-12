/*
 * rsd_sendq.c -- per-client bounded send queue
 *
 * Decouples the ring readers from network I/O: every entry holds a
 * malloc'd copy of the frame payload so a reader can overwrite its
 * frame buffer with the next ring frame without waiting for any send
 * thread. Self-contained (pthread only) so the unit suite compiles it
 * directly; the drop policies here are load-bearing for A/V behavior
 * on slow clients and are pinned by tests/test_sendq.c.
 */

#include <stdlib.h>
#include <string.h>

#include "rsd_sendq.h"

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

void rsd_sendq_release_entry(rsd_sendq_entry_t *e)
{
	free((void *)e->data);
	e->data = NULL;
}

void rsd_sendq_destroy(rsd_sendq_t *q)
{
	while (q->count > 0) {
		rsd_sendq_release_entry(&q->entries[q->tail]);
		q->tail = (q->tail + 1) % RSD_SENDQ_SLOTS;
		q->count--;
	}
	pthread_cond_destroy(&q->cond);
	pthread_mutex_destroy(&q->lock);
}

/*
 * Drop queued video after the client falls behind, but retain audio. The
 * caller will hold video at the next keyframe; keeping independently
 * decodable audio here avoids turning the video recovery into a sound gap.
 * Caller holds q->lock.
 */
static void sendq_drop_video_locked(rsd_sendq_t *q)
{
	rsd_sendq_entry_t audio[RSD_SENDQ_SLOTS];
	int audio_count = 0;

	while (q->count > 0) {
		rsd_sendq_entry_t entry = q->entries[q->tail];
		q->entries[q->tail].data = NULL;
		q->tail = (q->tail + 1) % RSD_SENDQ_SLOTS;
		q->count--;

		if (entry.type == RSD_FRAME_AUDIO) {
			audio[audio_count++] = entry;
		} else {
			q->drop_video++;
			rsd_sendq_release_entry(&entry);
		}
	}

	q->head = 0;
	q->tail = 0;
	for (int i = 0; i < audio_count; i++) {
		q->entries[q->head] = audio[i];
		q->head = (q->head + 1) % RSD_SENDQ_SLOTS;
		q->count++;
	}
}

/*
 * Remove the oldest audio entry from the queue, wherever it sits, and hand
 * ownership of it to the caller. Caller holds q->lock.
 *
 * Audio is not necessarily at the tail, so taking it means closing the gap
 * behind it. Shared by the two callers that need an audio entry out of the
 * middle of the queue: the interleaved drain, which sends it, and the overflow
 * path, which discards it.
 */
bool rsd_sendq_take_audio_locked(rsd_sendq_t *q, rsd_sendq_entry_t *out)
{
	for (int i = 0; i < q->count; i++) {
		int idx = (q->tail + i) % RSD_SENDQ_SLOTS;
		if (q->entries[idx].type != RSD_FRAME_AUDIO)
			continue;

		*out = q->entries[idx];
		q->entries[idx].data = NULL;
		/* Shift the entries behind it forward to fill the gap */
		for (int j = i; j > 0; j--) {
			int dst = (q->tail + j) % RSD_SENDQ_SLOTS;
			int src = (q->tail + j - 1) % RSD_SENDQ_SLOTS;
			q->entries[dst] = q->entries[src];
		}
		q->entries[q->tail].data = NULL;
		q->tail = (q->tail + 1) % RSD_SENDQ_SLOTS;
		q->count--;
		return true;
	}
	return false;
}

/*
 * Offset of the first VCL NAL in a 4-byte-start-code Annex B frame.
 * Returns len when no VCL NAL is found.
 */
static uint32_t first_vcl_offset(const uint8_t *data, uint32_t len, bool is_h265)
{
	const uint8_t *p = data;
	const uint8_t *end = data + len;

	while (p + 4 < end) {
		if (!(p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1)) {
			p++;
			continue;
		}
		uint8_t first = p[4];
		if (is_h265) {
			if (((first >> 1) & 0x3f) < 32)
				return (uint32_t)(p - data);
		} else {
			uint8_t t = first & 0x1f;
			if (t >= 1 && t <= 5)
				return (uint32_t)(p - data);
		}
		p += 4;
	}
	return len;
}

/*
 * Push a video frame onto the client's sendq. The data is copied so
 * the reader can immediately overwrite frame_buf with the next ring
 * frame — no barrier wait on the send thread. The old zero-copy
 * barrier capped throughput at 1 / send_latency on single-core SoCs
 * and was the root of the residual IDR clustering we saw even after
 * the crypto-path optimizations.
 *
 * A per-frame timecode SEI (sei_len > 0) is spliced into the copy
 * before the first VCL NAL, after any in-band SPS/PPS.
 */
int rsd_sendq_push_video(rsd_sendq_t *q, const uint8_t *data, uint32_t len, uint32_t rtp_ts,
			 const uint8_t *sei, uint32_t sei_len, bool is_h265)
{
	uint8_t *copy = malloc((size_t)len + sei_len);
	if (!copy)
		return -1;
	if (sei_len > 0) {
		uint32_t off = first_vcl_offset(data, len, is_h265);
		memcpy(copy, data, off);
		memcpy(copy + off, sei, sei_len);
		memcpy(copy + off + sei_len, data + off, len - off);
		len += sei_len;
	} else {
		memcpy(copy, data, len);
	}

	pthread_mutex_lock(&q->lock);
	if (q->shutdown) {
		pthread_mutex_unlock(&q->lock);
		free(copy);
		return -1;
	}

	if (q->count >= RSD_SENDQ_SLOTS) {
		q->overflows++;
		sendq_drop_video_locked(q);
		q->drop_video++; /* incoming pre-keyframe frame */
		pthread_mutex_unlock(&q->lock);
		free(copy);
		return RSD_SENDQ_DROPPED;
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
	return RSD_SENDQ_OK;
}

int rsd_sendq_push_audio(rsd_sendq_t *q, uint32_t codec, const uint8_t *data, uint32_t len,
			 uint32_t rtp_ts, int64_t capture_us)
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

	/*
	 * Overflow used to flush the whole queue. That is a defensible video
	 * policy -- a decoder that has lost frames wants the next IDR, not the
	 * frames in between -- and the wrong one for audio, where every chunk
	 * is independently useful: it discards up to RSD_SENDQ_SLOTS entries,
	 * a noticeable hole in the sound, to make room for 20ms of it. Worse, it
	 * takes the queued video with it.
	 *
	 * Drop the oldest audio chunk instead. That bounds the loss at one
	 * chunk per overflow and never touches video, so a slow client costs
	 * a click rather than a dropout plus a decode artifact. If the queue
	 * holds no audio at all, video is backed up badly enough that this
	 * frame has nowhere to go; drop the incoming one rather than start
	 * evicting video behind the send thread's back.
	 */
	if (q->count >= RSD_SENDQ_SLOTS) {
		rsd_sendq_entry_t victim;

		q->overflows++;
		q->drop_audio++;
		if (!rsd_sendq_take_audio_locked(q, &victim)) {
			pthread_mutex_unlock(&q->lock);
			free(copy);
			return RSD_SENDQ_DROPPED;
		}
		/* Freed under the lock, as sendq_drop_video_locked does: releasing it
		 * to free() would let another push refill the queue before the
		 * slot below is written. */
		rsd_sendq_release_entry(&victim);
	}

	rsd_sendq_entry_t *slot = &q->entries[q->head];
	slot->data = copy;
	slot->len = len;
	slot->rtp_ts = rtp_ts;
	slot->capture_us = capture_us;
	slot->type = RSD_FRAME_AUDIO;
	slot->codec = codec;
	slot->zerocopy = false; /* unused, kept for ABI compat */

	q->head = (q->head + 1) % RSD_SENDQ_SLOTS;
	q->count++;

	pthread_cond_signal(&q->cond);
	pthread_mutex_unlock(&q->lock);
	return RSD_SENDQ_OK;
}

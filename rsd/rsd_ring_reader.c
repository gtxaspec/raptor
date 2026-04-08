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

/*
 * Parse Annex B start codes and send each NALU via compy.
 *
 * The ring data is a concatenation of NALUs with 4-byte start codes
 * (0x00 0x00 0x00 0x01) as written by RVD's linearize_frame().
 */
static void rsd_send_video_frame(rsd_client_t *c, const uint8_t *data, uint32_t len,
				 uint32_t rtp_ts)
{
	if (!c->video.nal || !c->video.playing)
		return;

	bool is_h265 = (c->video_codec == 1); /* RSS_CODEC_H265 */
	int hdr_size = is_h265 ? 2 : 1;	      /* H.265=2 bytes, H.264=1 byte */

	const uint8_t *p = data;
	const uint8_t *end = data + len;

	while (p < end - 4) {
		/* Find 4-byte start code */
		if (!(p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1)) {
			p++;
			continue;
		}

		const uint8_t *nalu_start = p + 4;

		/* Find next start code or end */
		const uint8_t *nalu_end = end;
		for (const uint8_t *q = nalu_start + 1; q < end - 3; q++) {
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

		(void)!Compy_NalTransport_send_packet(c->video.nal, Compy_RtpTimestamp_Raw(rtp_ts),
						      nalu);

		p = nalu_end;
	}

	/* Periodic RTCP SR (every 5 seconds) */
	int64_t now = rss_timestamp_us();
	if (c->video.rtcp && now - c->video.last_rtcp > 5000000) {
		(void)!Compy_Rtcp_send_sr(c->video.rtcp);
		c->video.last_rtcp = now;
	}
}

/* Global server pointer (set in rsd_server_run before threads start) */
static rsd_server_t *g_srv_for_readers = NULL;

void rsd_set_server_for_readers(rsd_server_t *srv)
{
	g_srv_for_readers = srv;
}

void *rsd_video_reader_thread(void *arg)
{
	rsd_ring_ctx_t *rctx = arg;
	rsd_server_t *srv = g_srv_for_readers;
	int stream_idx = rctx->idx;

	/* Wall-clock video timestamps: derive from IMP's CLOCK_MONOTONIC_RAW
	 * timestamp, same clock source as audio. Both streams share the same
	 * timebase so inter-stream drift is zero by construction. */
	int64_t video_ts_epoch = 0;
	uint64_t last_write_seq = 0;
	int idle_count = 0;

	RSS_DEBUG("video reader[%d] started", stream_idx);
	while (*srv->running) {
		if (!rctx->ring) {
			/* Ring lost — wait for RVD to recreate it */
			rctx->ring = rss_ring_open(rctx->ring_name);
			if (!rctx->ring) {
				usleep(200000);
				continue;
			}
			const rss_ring_header_t *h = rss_ring_get_header(rctx->ring);
			if (rctx->frame_buf_size < h->data_size) {
				uint8_t *new_buf = malloc(h->data_size);
				if (!new_buf) {
					rss_ring_close(rctx->ring);
					rctx->ring = NULL;
					rctx->frame_buf_size = 0;
					continue;
				}
				free(rctx->frame_buf);
				rctx->frame_buf = new_buf;
				rctx->frame_buf_size = h->data_size;
			}
			rctx->read_seq = 0;
			last_write_seq = 0;
			idle_count = 0;
			video_ts_epoch = 0;
			RSS_DEBUG("video reader[%d] reconnected (%s)", stream_idx, rctx->ring_name);
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

		uint32_t length;
		rss_ring_slot_t meta;
		uint64_t read_seq = rctx->read_seq;

		if (!rctx->frame_buf)
			continue;

		/* Skip stale frames: if consumer is more than 1 frame behind,
		 * jump to latest to minimize latency. Request IDR since we
		 * skipped reference frames. */
		const rss_ring_header_t *hdr = rss_ring_get_header(rctx->ring);
		uint64_t write_seq = hdr->write_seq;
		if (write_seq > read_seq + 1 && read_seq > 0) {
			read_seq = write_seq - 1;
			rss_ring_request_idr(rctx->ring);
		}

		ret = rss_ring_read(rctx->ring, &read_seq, rctx->frame_buf, rctx->frame_buf_size,
				    &length, &meta);
		if (ret == RSS_EOVERFLOW) {
			rctx->read_seq = read_seq;
			rss_ring_request_idr(rctx->ring);
			continue;
		}
		if (ret != 0)
			continue;

		rctx->read_seq = read_seq;

		if (video_ts_epoch == 0)
			video_ts_epoch = meta.timestamp;
		uint32_t rtp_ts =
			(uint32_t)((uint64_t)(meta.timestamp - video_ts_epoch) * 90000 / 1000000);

		pthread_mutex_lock(&srv->clients_lock);
		for (int i = 0; i < srv->client_count; i++) {
			rsd_client_t *c = srv->clients[i];
			if (!c || !c->video.playing)
				continue;
			if (c->stream_idx != stream_idx)
				continue;

			if (c->waiting_keyframe) {
				if (!meta.is_key)
					continue;
				c->waiting_keyframe = false;
				c->video_ts_offset = rtp_ts;
				c->video_ts_base_set = true;
				RSS_DEBUG("client[%d] got keyframe (ts_offset=%u)", stream_idx,
					  rtp_ts);
			}

			uint32_t client_ts = rtp_ts - c->video_ts_offset;
			rsd_send_video_frame(c, rctx->frame_buf, length, client_ts);
		}
		pthread_mutex_unlock(&srv->clients_lock);
	}

	RSS_DEBUG("video reader[%d] exiting", stream_idx);
	return NULL;
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

	/* Periodic RTCP SR */
	int64_t now = rss_timestamp_us();
	if (c->audio.rtcp && now - c->audio.last_rtcp > 5000000) {
		(void)!Compy_Rtcp_send_sr(c->audio.rtcp);
		c->audio.last_rtcp = now;
	}
}

void *rsd_audio_reader_thread(void *arg)
{
	rsd_server_t *srv = arg;

	RSS_DEBUG("audio reader thread started");

	uint32_t audio_codec = 0, rtp_clock = 0;
	uint8_t audio_buf[4096];
	int64_t audio_ts_epoch = 0;
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

	while (*srv->running) {
		if (!srv->ring_audio) {
			/* Ring lost — wait for RAD to recreate it */
			srv->ring_audio = rss_ring_open("audio");
			if (!srv->ring_audio) {
				usleep(200000);
				continue;
			}
			const rss_ring_header_t *ahdr = rss_ring_get_header(srv->ring_audio);
			audio_codec = ahdr->codec;
			uint32_t audio_clock = ahdr->fps_num;
			rtp_clock = (audio_codec == RSD_CODEC_OPUS) ? 48000 : audio_clock;
			srv->audio_read_seq = ahdr->write_seq;
			audio_ts_epoch = 0;
			last_write_seq = 0;
			idle_count = 0;

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

			/* Derive RTP timestamp from wall-clock (µs since
			 * IMP_System_Init, CLOCK_MONOTONIC_RAW). */
			if (audio_ts_epoch == 0)
				audio_ts_epoch = meta.timestamp;
			uint32_t rtp_ts = (uint32_t)((uint64_t)(meta.timestamp - audio_ts_epoch) *
						     rtp_clock / 1000000);

			pthread_mutex_lock(&srv->clients_lock);
			for (int i = 0; i < srv->client_count; i++) {
				rsd_client_t *c = srv->clients[i];
				if (!c || !c->audio.playing)
					continue;

				/* Gate audio on video keyframe — don't send audio
				 * until the client has received its first video
				 * keyframe, so both RTP timelines start together. */
				if (c->video.playing && !c->video_ts_base_set)
					continue;

				if (!c->audio_ts_base_set) {
					c->audio_ts_offset = rtp_ts;
					c->audio_ts_base_set = true;
				}
				uint32_t client_ts = rtp_ts - c->audio_ts_offset;
				rsd_send_audio_frame(c, audio_codec, audio_buf, length, client_ts);
			}
			pthread_mutex_unlock(&srv->clients_lock);
		}
	}

	RSS_DEBUG("audio reader thread exiting");
	return NULL;
}

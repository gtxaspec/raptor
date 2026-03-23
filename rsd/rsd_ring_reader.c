/*
 * rsd_ring_reader.c -- SHM ring consumer + frame distribution
 *
 * A dedicated thread reads frames from the SHM ring and distributes
 * them to all playing RTSP clients via their compy NalTransport.
 */

#include <stdio.h>
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

	/* Get FPS from ring header for counter-based timestamps */
	const rss_ring_header_t *vhdr = rss_ring_get_header(rctx->ring);
	uint32_t video_ts_inc = 90000 / (vhdr->fps_num ? vhdr->fps_num : 25);
	uint32_t video_rtp_ts = 0;

	RSS_INFO("video reader[%d] started (ts_inc=%u)", stream_idx, video_ts_inc);

	while (*srv->running) {
		int ret = rss_ring_wait(rctx->ring, 100);
		if (ret != 0)
			continue;

		const uint8_t *data;
		uint32_t length;
		rss_ring_slot_t meta;
		uint64_t read_seq = rctx->read_seq;

		ret = rss_ring_read(rctx->ring, &read_seq, &data, &length, &meta);
		if (ret == RSS_EOVERFLOW) {
			rctx->read_seq = read_seq;
			continue;
		}
		if (ret != 0)
			continue;

		rctx->read_seq = read_seq;

		if (length > rctx->frame_buf_size || !rctx->frame_buf)
			continue;
		memcpy(rctx->frame_buf, data, length);

		/* Counter-based RTP timestamp for perfect monotonicity */
		uint32_t rtp_ts = video_rtp_ts;
		video_rtp_ts += video_ts_inc;

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
				RSS_DEBUG("client[%d] got keyframe", stream_idx);
			}

			rsd_send_video_frame(c, rctx->frame_buf, length, rtp_ts);
		}
		pthread_mutex_unlock(&srv->clients_lock);
	}

	RSS_INFO("video reader[%d] exiting", stream_idx);
	return NULL;
}

/* ── Audio ring reader thread ── */

static void rsd_send_audio_frame(rsd_client_t *c, const uint8_t *data, uint32_t len,
				 uint32_t rtp_ts)
{
	if (!c->audio.rtp || !c->audio.playing)
		return;

	(void)!Compy_RtpTransport_send_packet(c->audio.rtp, Compy_RtpTimestamp_Raw(rtp_ts),
					      false,		 /* no marker */
					      U8Slice99_empty(), /* no payload header */
					      U8Slice99_new((uint8_t *)data, len));

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

	RSS_INFO("audio reader thread started");

	/* Get clock rate from ring header */
	const rss_ring_header_t *ahdr = rss_ring_get_header(srv->ring_audio);
	uint32_t audio_clock = ahdr->fps_num; /* fps_num holds sample_rate */

	/* Buffer for audio frames (L16@16kHz = 640 bytes/20ms) */
	uint8_t audio_buf[4096];
	/*
	 * Use a sample counter for audio RTP timestamps instead of
	 * converting HAL microsecond timestamps. HAL timestamps have
	 * sub-millisecond jitter that causes non-monotonic DTS at the
	 * receiver. A counter incremented by samples_per_frame gives
	 * perfectly monotonic timestamps with zero jitter.
	 */
	uint32_t audio_rtp_ts = 0;
	uint32_t samples_per_frame = audio_clock / 50; /* 20ms */

	while (*srv->running) {
		int ret = rss_ring_wait(srv->ring_audio, 100);
		if (ret != 0)
			continue;

		for (int burst = 0; burst < 16; burst++) {
			const uint8_t *data;
			uint32_t length;
			rss_ring_slot_t meta;
			uint64_t read_seq = srv->audio_read_seq;

			ret = rss_ring_read(srv->ring_audio, &read_seq, &data, &length, &meta);
			if (ret == RSS_EOVERFLOW) {
				srv->audio_read_seq = read_seq;
				break;
			}
			if (ret != 0)
				break;

			srv->audio_read_seq = read_seq;

			if (length > sizeof(audio_buf))
				continue;
			memcpy(audio_buf, data, length);

			uint32_t rtp_ts = audio_rtp_ts;
			audio_rtp_ts += samples_per_frame;

			pthread_mutex_lock(&srv->clients_lock);
			for (int i = 0; i < srv->client_count; i++) {
				rsd_client_t *c = srv->clients[i];
				if (!c || !c->audio.playing)
					continue;

				rsd_send_audio_frame(c, audio_buf, length, rtp_ts);
			}
			pthread_mutex_unlock(&srv->clients_lock);
		}
	}

	RSS_INFO("audio reader thread exiting");
	return NULL;
}

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
void rsd_send_video_frame(rsd_client_t *c, const uint8_t *data,
			  uint32_t len, int64_t timestamp,
			  bool is_key __attribute__((unused)))
{
	if (!c->video.nal || !c->video.playing) return;

	/* Convert timestamp from microseconds to RTP clock (90kHz) */
	uint32_t rtp_ts = (uint32_t)((timestamp * 90000LL) / 1000000LL);

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
		if (nalu_len < 1) {
			p = nalu_end;
			continue;
		}

		/* Parse H.264 NAL header (first byte) */
		uint8_t header_byte = nalu_start[0];

		Compy_NalUnit nalu = {
			.header = Compy_NalHeader_H264(
				Compy_H264NalHeader_parse(header_byte)),
			.payload = U8Slice99_new((uint8_t *)(nalu_start + 1), nalu_len - 1),
		};

		(void)!Compy_NalTransport_send_packet(
			c->video.nal,
			Compy_RtpTimestamp_Raw(rtp_ts),
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

void *rsd_ring_reader_thread(void *arg)
{
	rsd_server_t *srv = arg;

	RSS_INFO("ring reader thread started");

	while (*srv->running) {
		/* Wait for new frame */
		int ret = rss_ring_wait(srv->ring_main, 100);
		if (ret != 0) continue;

		const uint8_t *data;
		uint32_t length;
		rss_ring_slot_t meta;
		uint64_t read_seq = srv->ring_read_seq;

		ret = rss_ring_read(srv->ring_main, &read_seq, &data,
				    &length, &meta);
		if (ret == RSS_EOVERFLOW) {
			RSS_DEBUG("ring reader: overflow, catching up");
			srv->ring_read_seq = read_seq;
			continue;
		}
		if (ret != 0) continue;

		srv->ring_read_seq = read_seq;

		/* Copy frame data to local buffer before distributing.
		 * The ring's data region is shared memory that the producer
		 * can overwrite at any time. */
		if (length > srv->frame_buf_size || !srv->frame_buf)
			continue;
		memcpy(srv->frame_buf, data, length);

		/* Distribute frame to all playing clients */
		pthread_mutex_lock(&srv->clients_lock);
		for (int i = 0; i < srv->client_count; i++) {
			rsd_client_t *c = srv->clients[i];
			if (!c || !c->video.playing) continue;

			/* Wait for keyframe before sending first frame */
			if (c->waiting_keyframe) {
				if (!meta.is_key) continue;
				c->waiting_keyframe = false;
				RSS_DEBUG("client got keyframe, starting stream");
			}

			rsd_send_video_frame(c, srv->frame_buf, length,
					     meta.timestamp, meta.is_key);
		}
		pthread_mutex_unlock(&srv->clients_lock);
	}

	RSS_INFO("ring reader thread exiting");
	return NULL;
}

/* ── Audio ring reader thread ── */

static void rsd_send_audio_frame(rsd_client_t *c, const uint8_t *data,
				 uint32_t len, int64_t timestamp)
{
	if (!c->audio.rtp || !c->audio.playing) return;

	/* Convert timestamp from microseconds to RTP clock (8kHz for PCMU) */
	uint32_t rtp_ts = (uint32_t)((timestamp * RSD_AUDIO_CLOCK) / 1000000LL);

	(void)!Compy_RtpTransport_send_packet(
		c->audio.rtp,
		Compy_RtpTimestamp_Raw(rtp_ts),
		false,                        /* no marker */
		U8Slice99_empty(),            /* no payload header */
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

	/* Small buffer for audio frames (PCMU, ~160 bytes per 20ms) */
	uint8_t audio_buf[4096];

	while (*srv->running) {
		int ret = rss_ring_wait(srv->ring_audio, 100);
		if (ret != 0) continue;

		/* Drain all available audio frames per wake-up.
		 * Audio produces 50 frames/sec; reading one-at-a-time
		 * with poll overhead causes the consumer to fall behind. */
		for (int burst = 0; burst < 16; burst++) {
			const uint8_t *data;
			uint32_t length;
			rss_ring_slot_t meta;
			uint64_t read_seq = srv->audio_read_seq;

			ret = rss_ring_read(srv->ring_audio, &read_seq, &data,
					    &length, &meta);
			if (ret == RSS_EOVERFLOW) {
				srv->audio_read_seq = read_seq;
				break;
			}
			if (ret != 0) break;  /* no more frames */

			srv->audio_read_seq = read_seq;

			if (length > sizeof(audio_buf)) continue;
			memcpy(audio_buf, data, length);

			pthread_mutex_lock(&srv->clients_lock);
			for (int i = 0; i < srv->client_count; i++) {
				rsd_client_t *c = srv->clients[i];
				if (!c || !c->audio.playing) continue;

				rsd_send_audio_frame(c, audio_buf, length,
						     meta.timestamp);
			}
			pthread_mutex_unlock(&srv->clients_lock);
		}
	}

	RSS_INFO("audio reader thread exiting");
	return NULL;
}

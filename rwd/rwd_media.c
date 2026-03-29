/*
 * rwd_media.c -- Ring reader → SRTP sender
 *
 * Implements a sendto-based Compy_Transport for multiplexed UDP,
 * sets up per-client SRTP/RTP/NAL stacks, and runs reader threads
 * that distribute video+audio frames to all WebRTC clients.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sys/socket.h>

#include "rwd.h"

/* ── sendto-based UDP transport (Compy_Transport interface) ──
 *
 * Unlike compy's connected UDP transport, this uses sendto() with
 * a stored destination address, allowing all clients to share one
 * socket. */

typedef struct {
	int fd;
	struct sockaddr_storage addr;
	socklen_t addr_len;
} RwdUdpSender;

declImpl(Compy_Transport, RwdUdpSender);

static int RwdUdpSender_transmit(VSelf, Compy_IoVecSlice bufs)
{
	VSELF(RwdUdpSender);
	struct msghdr msg = {
		.msg_name = (void *)&self->addr,
		.msg_namelen = self->addr_len,
		.msg_iov = bufs.ptr,
		.msg_iovlen = bufs.len,
	};
	ssize_t ret = sendmsg(self->fd, &msg, 0);
	return ret >= 0 ? 0 : -1;
}

static bool RwdUdpSender_is_full(VSelf)
{
	VSELF(RwdUdpSender);
	(void)self;
	return false;
}

static void RwdUdpSender_drop(VSelf)
{
	VSELF(RwdUdpSender);
	/* Don't close fd — it's the shared UDP socket */
	free(self);
}

impl(Compy_Droppable, RwdUdpSender);
impl(Compy_Transport, RwdUdpSender);

Compy_Transport rwd_transport_sendto(int fd, const struct sockaddr_storage *addr,
				     socklen_t addr_len)
{
	RwdUdpSender *s = calloc(1, sizeof(*s));
	if (!s) {
		RSS_ERROR("media: failed to allocate UDP sender");
		/* Return a null transport — caller must check */
		return (Compy_Transport){0};
	}
	s->fd = fd;
	memcpy(&s->addr, addr, addr_len);
	s->addr_len = addr_len;
	return DYN(RwdUdpSender, Compy_Transport, s);
}

/* ── Set up per-client SRTP/RTP/NAL stack ── */

int rwd_media_setup(rwd_client_t *c)
{
	Compy_SrtpKeyMaterial send_key, recv_key;
	if (rwd_dtls_export_srtp_keys(c, &send_key, &recv_key) != 0)
		return -1;

	rwd_server_t *srv = c->server;
	Compy_SrtpSuite suite = Compy_SrtpSuite_AES_CM_128_HMAC_SHA1_80;

	/* Video: UDP → SRTP → RTP → NAL */
	Compy_Transport udp_v = rwd_transport_sendto(srv->udp_fd, &c->addr, c->addr_len);
	if (!udp_v.self) {
		RSS_ERROR("media: failed to create video UDP transport");
		return -1;
	}
	c->srtp_video = compy_transport_srtp(udp_v, suite, &send_key);

	int video_pt = c->offer.video_pt > 0 ? c->offer.video_pt : RWD_VIDEO_PT;
	c->rtp_video = Compy_RtpTransport_new(c->srtp_video, (uint8_t)video_pt, RWD_VIDEO_CLOCK);
	Compy_RtpTransport_set_ssrc(c->rtp_video, c->video_ssrc);
	/* sdes:mid extension for BUNDLE demux (pion/go2rtc requires it) */
	if (c->offer.mid_ext_id > 0 && c->offer.mid_ext_id <= 14) {
		const char *vmid = c->offer.mid_video[0] ? c->offer.mid_video : "0";
		Compy_RtpTransport_set_extension(c->rtp_video, (uint8_t)c->offer.mid_ext_id,
						 (const uint8_t *)vmid, (uint8_t)strlen(vmid));
	}
	c->nal_video = Compy_NalTransport_new(c->rtp_video);

	/* Video RTCP (SRTCP) */
	Compy_Transport udp_rtcp_v = rwd_transport_sendto(srv->udp_fd, &c->addr, c->addr_len);
	if (udp_rtcp_v.self) {
		Compy_Transport srtcp_v = compy_transport_srtcp(udp_rtcp_v, suite, &send_key);
		c->rtcp_video = Compy_Rtcp_new(c->rtp_video, srtcp_v, "raptor");
	}

	/* Audio: UDP → SRTP → RTP (no NAL for audio) */
	if (c->offer.has_audio && c->offer.audio_pt >= 0 && srv->has_audio) {
		Compy_Transport udp_a = rwd_transport_sendto(srv->udp_fd, &c->addr, c->addr_len);
		if (udp_a.self) {
			c->srtp_audio = compy_transport_srtp(udp_a, suite, &send_key);

			int audio_pt = c->offer.audio_pt > 0 ? c->offer.audio_pt : RWD_AUDIO_PT;
			c->rtp_audio = Compy_RtpTransport_new(c->srtp_audio, (uint8_t)audio_pt,
							      RWD_AUDIO_CLOCK);
			Compy_RtpTransport_set_ssrc(c->rtp_audio, c->audio_ssrc);
			/* sdes:mid extension for audio */
			if (c->offer.mid_ext_id > 0 && c->offer.mid_ext_id <= 14) {
				const char *amid = c->offer.mid_audio[0] ? c->offer.mid_audio : "1";
				Compy_RtpTransport_set_extension(
					c->rtp_audio, (uint8_t)c->offer.mid_ext_id,
					(const uint8_t *)amid, (uint8_t)strlen(amid));
			}

			/* Audio RTCP */
			Compy_Transport udp_rtcp_a =
				rwd_transport_sendto(srv->udp_fd, &c->addr, c->addr_len);
			if (udp_rtcp_a.self) {
				Compy_Transport srtcp_a =
					compy_transport_srtcp(udp_rtcp_a, suite, &send_key);
				c->rtcp_audio = Compy_Rtcp_new(c->rtp_audio, srtcp_a, "raptor");
			}
		}
	}

	c->media_ready = true;
	c->sending = true;
	c->waiting_keyframe = true;

	/* Request IDR so we get a keyframe quickly */
	int si = c->stream_idx;
	if (si >= 0 && si < RWD_STREAM_COUNT && srv->video_rings[si])
		rss_ring_request_idr(srv->video_rings[si]);

	RSS_INFO("media: SRTP stack ready for %s (IDR requested)", c->session_id);
	return 0;
}

void rwd_media_teardown(rwd_client_t *c)
{
	c->sending = false;
	c->media_ready = false;

	/* Drop order matters:
	 * 1. RTCP first (borrows RTP transport, owns SRTCP transport)
	 * 2. NAL → RTP → SRTP → UDP chain for video
	 * 3. RTP → SRTP → UDP chain for audio */
	if (c->rtcp_audio)
		VCALL(DYN(Compy_Rtcp, Compy_Droppable, c->rtcp_audio), drop);
	if (c->rtcp_video)
		VCALL(DYN(Compy_Rtcp, Compy_Droppable, c->rtcp_video), drop);
	if (c->nal_video)
		VCALL(DYN(Compy_NalTransport, Compy_Droppable, c->nal_video), drop);
	else if (c->rtp_video)
		VCALL(DYN(Compy_RtpTransport, Compy_Droppable, c->rtp_video), drop);
	if (c->rtp_audio)
		VCALL(DYN(Compy_RtpTransport, Compy_Droppable, c->rtp_audio), drop);

	c->nal_video = NULL;
	c->rtp_video = NULL;
	c->rtp_audio = NULL;
	c->rtcp_video = NULL;
	c->rtcp_audio = NULL;
	memset(&c->srtp_video, 0, sizeof(c->srtp_video));
	memset(&c->srtp_audio, 0, sizeof(c->srtp_audio));
}

/* ── Parse Annex B and send NALUs via SRTP ── */

static void rwd_send_video_frame(rwd_client_t *c, const uint8_t *data, uint32_t len,
				 uint32_t rtp_ts)
{
	if (!c->nal_video || !c->sending)
		return;

	/* First pass: collect SPS/PPS for STAP-A aggregation */
	const uint8_t *sps_data = NULL, *pps_data = NULL;
	uint32_t sps_len = 0, pps_len = 0;
	const uint8_t *p = data;
	const uint8_t *end = data + len;

	while (p < end - 4) {
		if (!(p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1)) {
			p++;
			continue;
		}
		const uint8_t *ns = p + 4;
		const uint8_t *ne = end;
		for (const uint8_t *q = ns + 1; q < end - 3; q++) {
			if (q[0] == 0 && q[1] == 0 && q[2] == 0 && q[3] == 1) {
				ne = q;
				break;
			}
		}
		uint32_t nl = (uint32_t)(ne - ns);
		if (nl >= 1) {
			uint8_t nt = ns[0] & 0x1F;
			if (nt == 7) {
				sps_data = ns;
				sps_len = nl;
			}
			if (nt == 8) {
				pps_data = ns;
				pps_len = nl;
			}
		}
		p = ne;
	}

	/* Send STAP-A with SPS+PPS if both present (marker=false) */
	if (sps_data && pps_data && sps_len + pps_len + 5 < 1200) {
		uint8_t stap[1200];
		uint32_t off = 0;
		stap[off++] = (sps_data[0] & 0xE0) | 24; /* STAP-A: NRI from SPS, type=24 */
		stap[off++] = (uint8_t)(sps_len >> 8);
		stap[off++] = (uint8_t)(sps_len);
		memcpy(stap + off, sps_data, sps_len);
		off += sps_len;
		stap[off++] = (uint8_t)(pps_len >> 8);
		stap[off++] = (uint8_t)(pps_len);
		memcpy(stap + off, pps_data, pps_len);
		off += pps_len;

		(void)!Compy_RtpTransport_send_packet(c->rtp_video, Compy_RtpTimestamp_Raw(rtp_ts),
						      false, U8Slice99_empty(),
						      U8Slice99_new(stap, off));
	}

	/* Second pass: send picture NALUs (skip SPS/PPS) */
	p = data;
	while (p < end - 4) {
		if (!(p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1)) {
			p++;
			continue;
		}
		const uint8_t *nalu_start = p + 4;
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

		uint8_t nal_type = nalu_start[0] & 0x1F;
		if (nal_type == 7 || nal_type == 8) {
			p = nalu_end;
			continue;
		} /* skip SPS/PPS */

		Compy_NalUnit nalu = {
			.header = Compy_NalHeader_H264(Compy_H264NalHeader_parse(nalu_start[0])),
			.payload = U8Slice99_new((uint8_t *)(nalu_start + 1), nalu_len - 1),
		};
		(void)!Compy_NalTransport_send_packet(c->nal_video, Compy_RtpTimestamp_Raw(rtp_ts),
						      nalu);
		p = nalu_end;
	}

	/* Periodic RTCP SR (every 5 seconds) */
	if (c->rtcp_video) {
		int64_t now = rss_timestamp_us();
		if (now - c->last_rtcp_video > 5000000) {
			(void)!Compy_Rtcp_send_sr(c->rtcp_video);
			c->last_rtcp_video = now;
		}
	}
}

static void rwd_send_audio_frame(rwd_client_t *c, uint32_t codec, const uint8_t *data, uint32_t len,
				 uint32_t rtp_ts)
{
	if (!c->rtp_audio || !c->sending)
		return;

	/* Opus: raw packet, marker=true */
	(void)!Compy_RtpTransport_send_packet(c->rtp_audio, Compy_RtpTimestamp_Raw(rtp_ts), true,
					      U8Slice99_empty(),
					      U8Slice99_new((uint8_t *)data, len));
	(void)codec;

	/* Periodic RTCP SR (every 5 seconds) */
	if (c->rtcp_audio) {
		int64_t now = rss_timestamp_us();
		if (now - c->last_rtcp_audio > 5000000) {
			(void)!Compy_Rtcp_send_sr(c->rtcp_audio);
			c->last_rtcp_audio = now;
		}
	}
}

/* ── Video reader thread ──
 *
 * Reads from both main (stream 0) and sub (stream 1) rings,
 * dispatching frames to clients based on their stream_idx. */

static const char *ring_names[RWD_STREAM_COUNT] = {"main", "sub"};

void *rwd_video_reader_thread(void *arg)
{
	rwd_server_t *srv = arg;
	uint32_t video_ts_inc[RWD_STREAM_COUNT] = {0};
	uint32_t video_rtp_ts[RWD_STREAM_COUNT] = {0};

	/* Wait for at least the main ring */
	while (*srv->running && !srv->video_rings[0]) {
		for (int s = 0; s < RWD_STREAM_COUNT; s++) {
			if (!srv->video_rings[s])
				srv->video_rings[s] = rss_ring_open(ring_names[s]);
		}
		if (!srv->video_rings[0])
			usleep(500000);
	}
	if (!srv->video_rings[0])
		return NULL;

	/* Initialize per-stream state */
	for (int s = 0; s < RWD_STREAM_COUNT; s++) {
		if (!srv->video_rings[s])
			continue;
		const rss_ring_header_t *vhdr = rss_ring_get_header(srv->video_rings[s]);
		uint32_t fps = vhdr->fps_num ? vhdr->fps_num : 25;
		video_ts_inc[s] = RWD_VIDEO_CLOCK / fps;

		srv->video_buf_sizes[s] =
			vhdr->data_size / (vhdr->slot_count ? vhdr->slot_count : 1);
		if (srv->video_buf_sizes[s] < 256 * 1024)
			srv->video_buf_sizes[s] = 256 * 1024;
		srv->video_bufs[s] = malloc(srv->video_buf_sizes[s]);
		if (!srv->video_bufs[s]) {
			RSS_ERROR("media: failed to allocate video buffer[%d]", s);
			continue;
		}
		srv->video_read_seq[s] = vhdr->write_seq;
		RSS_INFO("media: video reader[%d] started (fps=%u ts_inc=%u)", s, fps,
			 video_ts_inc[s]);
	}

	while (*srv->running) {
		/* Poll both rings (short timeout so we alternate quickly) */
		for (int s = 0; s < RWD_STREAM_COUNT; s++) {
			if (!srv->video_rings[s] || !srv->video_bufs[s])
				continue;

			int ret = rss_ring_wait(srv->video_rings[s], 50);
			if (ret != 0)
				continue;

			uint32_t length;
			rss_ring_slot_t meta;
			uint64_t read_seq = srv->video_read_seq[s];

			ret = rss_ring_read(srv->video_rings[s], &read_seq, srv->video_bufs[s],
					    srv->video_buf_sizes[s], &length, &meta);
			if (ret == RSS_EOVERFLOW) {
				srv->video_read_seq[s] = read_seq;
				rss_ring_request_idr(srv->video_rings[s]);
				continue;
			}
			if (ret != 0)
				continue;

			srv->video_read_seq[s] = read_seq;

			uint32_t rtp_ts = video_rtp_ts[s];
			video_rtp_ts[s] += video_ts_inc[s];

			pthread_mutex_lock(&srv->clients_lock);
			for (int i = 0; i < RWD_MAX_CLIENTS; i++) {
				rwd_client_t *c = srv->clients[i];
				if (!c || !c->sending || c->stream_idx != s)
					continue;

				if (c->waiting_keyframe) {
					if (!meta.is_key)
						continue;
					c->waiting_keyframe = false;
					c->video_ts_offset = rtp_ts;
					c->video_ts_base_set = true;
					RSS_INFO("media: client[%d] got keyframe, starting send",
						 s);
				}

				uint32_t client_ts = rtp_ts - c->video_ts_offset;
				rwd_send_video_frame(c, srv->video_bufs[s], length, client_ts);
			}
			pthread_mutex_unlock(&srv->clients_lock);
		}
	}

	for (int s = 0; s < RWD_STREAM_COUNT; s++) {
		free(srv->video_bufs[s]);
		srv->video_bufs[s] = NULL;
	}
	RSS_INFO("media: video reader exiting");
	return NULL;
}

/* ── Audio reader thread ── */

void *rwd_audio_reader_thread(void *arg)
{
	rwd_server_t *srv = arg;

	/* Wait for audio ring.
	 * NOTE: srv->audio_ring and srv->has_audio are set once here at
	 * thread start and never changed afterward. The word-aligned pointer
	 * write is atomic on MIPS, and has_audio is set before any client
	 * can exist to read it, so no additional synchronization is needed. */
	while (*srv->running && !srv->audio_ring) {
		srv->audio_ring = rss_ring_open("audio");
		if (!srv->audio_ring)
			usleep(2000000); /* retry every 2s */
	}
	if (!srv->audio_ring)
		return NULL;

	srv->has_audio = true;

	const rss_ring_header_t *ahdr = rss_ring_get_header(srv->audio_ring);
	uint32_t audio_codec = ahdr->codec;
	uint32_t samples_per_frame = 960; /* Opus: 20ms at 48kHz = 960 */
	uint32_t audio_rtp_ts = 0;

	uint8_t audio_buf[4096];

	/* Start from latest */
	srv->audio_read_seq = ahdr->write_seq;

	RSS_INFO("media: audio reader started (codec=%u)", audio_codec);

	while (*srv->running) {
		int ret = rss_ring_wait(srv->audio_ring, 100);
		if (ret != 0)
			continue;

		for (int burst = 0; burst < 16; burst++) {
			uint32_t length;
			rss_ring_slot_t meta;
			uint64_t read_seq = srv->audio_read_seq;

			ret = rss_ring_read(srv->audio_ring, &read_seq, audio_buf,
					    sizeof(audio_buf), &length, &meta);
			if (ret == RSS_EOVERFLOW) {
				srv->audio_read_seq = read_seq;
				break;
			}
			if (ret != 0)
				break;

			srv->audio_read_seq = read_seq;
			uint32_t rtp_ts = audio_rtp_ts;
			audio_rtp_ts += samples_per_frame;

			pthread_mutex_lock(&srv->clients_lock);
			for (int i = 0; i < RWD_MAX_CLIENTS; i++) {
				rwd_client_t *c = srv->clients[i];
				if (!c || !c->sending || !c->rtp_audio)
					continue;

				if (!c->audio_ts_base_set) {
					c->audio_ts_offset = rtp_ts;
					c->audio_ts_base_set = true;
				}
				uint32_t client_ts = rtp_ts - c->audio_ts_offset;
				rwd_send_audio_frame(c, audio_codec, audio_buf, length, client_ts);
			}
			pthread_mutex_unlock(&srv->clients_lock);
		}
	}

	RSS_INFO("media: audio reader exiting");
	return NULL;
}

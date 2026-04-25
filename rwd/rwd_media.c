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
#include <opus/opus.h>
#ifdef RAPTOR_AAC
#include <aacdec.h>
#endif

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

/* ── Backchannel audio receiver (browser mic → speaker ring) ── */

typedef struct {
	rss_ring_t **speaker_ring_ptr;
	OpusDecoder *opus_dec;
} rwd_bc_recv_t;

static int16_t ulaw_decode(uint8_t ulaw)
{
	ulaw = ~ulaw;
	int sign = (ulaw & 0x80);
	int exponent = (ulaw >> 4) & 0x07;
	int mantissa = ulaw & 0x0f;
	int magnitude = ((mantissa << 3) + 0x84) << exponent;
	magnitude -= 0x84;
	return (int16_t)(sign ? -magnitude : magnitude);
}

static int16_t alaw_decode(uint8_t alaw)
{
	alaw ^= 0x55;
	int sign = alaw & 0x80;
	int exponent = (alaw >> 4) & 0x07;
	int mantissa = alaw & 0x0f;
	int magnitude =
		(exponent == 0) ? (mantissa << 4) + 8 : ((mantissa << 4) + 264) << (exponent - 1);
	return (int16_t)(sign ? magnitude : -magnitude);
}

static void rwd_bc_recv_t_on_audio(VSelf, uint8_t payload_type, uint32_t timestamp, uint32_t ssrc,
				   U8Slice99 payload)
{
	VSELF(rwd_bc_recv_t);
	(void)timestamp;
	(void)ssrc;

	rss_ring_t **ring_ptr = self->speaker_ring_ptr;
	if (!*ring_ptr) {
		/* Open existing ring if RAD already has it mapped, only
		 * create new if none exists — avoids stale mmap issue
		 * where RAD holds an old deleted SHM inode. */
		*ring_ptr = rss_ring_open("speaker");
		if (!*ring_ptr)
			*ring_ptr = rss_ring_create("speaker", 16, 64 * 1024);
		if (!*ring_ptr) {
			RSS_WARN("backchannel: failed to open/create speaker ring");
			return;
		}
		rss_ring_set_stream_info(*ring_ptr, 0x11, 0, 0, 0, 16000, 1, 0, 0);
		RSS_DEBUG("backchannel: speaker ring ready");
	}

	int16_t pcm48[960]; /* 20ms at 48kHz */
	int16_t pcm16[320]; /* 20ms at 16kHz */

	if (payload_type == 0) {
		/* PCMU/8000 → PCM16, upsample 8kHz→16kHz (duplicate samples) */
		int16_t pcm_up[1920];
		int n = (int)payload.len;
		if (n > 960)
			n = 960;
		for (int i = 0; i < n; i++) {
			int16_t s = ulaw_decode(payload.ptr[i]);
			pcm_up[i * 2] = s;
			pcm_up[i * 2 + 1] = s;
		}
		rss_ring_publish(*ring_ptr, (const uint8_t *)pcm_up, n * 4, rss_timestamp_us(), 0,
				 0);
	} else if (self->opus_dec && payload.len > 0) {
		/* Opus → PCM16 at 48kHz, downsample 3:1 to 16kHz */
		int samples = opus_decode(self->opus_dec, payload.ptr, (opus_int32)payload.len,
					  pcm48, 960, 0);
		if (samples > 0) {
			int out_samples = samples / 3;
			for (int i = 0; i < out_samples; i++)
				pcm16[i] = pcm48[i * 3];
			rss_ring_publish(*ring_ptr, (const uint8_t *)pcm16, out_samples * 2,
					 rss_timestamp_us(), 0, 0);
		}
	}
}

static void rwd_bc_recv_t_drop(VSelf)
{
	VSELF(rwd_bc_recv_t);
	if (self->opus_dec)
		opus_decoder_destroy(self->opus_dec);
}

impl(Compy_AudioReceiver, rwd_bc_recv_t);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-const-variable"
impl(Compy_Droppable, rwd_bc_recv_t);
#pragma GCC diagnostic pop

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

	/* Audio: UDP → SRTP → RTP (no NAL for audio)
	 * Check audio_ring directly instead of has_audio flag — the flag may
	 * not be set yet if the audio reader thread is still starting up. */
	if (c->offer.has_audio && c->offer.audio_pt >= 0 && srv->audio_ring) {
		Compy_Transport udp_a = rwd_transport_sendto(srv->udp_fd, &c->addr, c->addr_len);
		if (udp_a.self) {
			c->srtp_audio = compy_transport_srtp(udp_a, suite, &send_key);

			int audio_pt = srv->wire_pt;
			int audio_clock = srv->wire_clock;
			c->rtp_audio = Compy_RtpTransport_new(c->srtp_audio, (uint8_t)audio_pt,
							      audio_clock);
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

	/* Backchannel: SRTP recv context + Compy_Backchannel for incoming audio.
	 * Accept Opus (PT from offer) — browsers send Opus over WebRTC.
	 * Also accept PCMU (PT 0) as fallback. */
	rwd_bc_recv_t *bc = calloc(1, sizeof(rwd_bc_recv_t));
	if (bc) {
		bc->speaker_ring_ptr = &c->speaker_ring;

		int err;
		bc->opus_dec = opus_decoder_create(48000, 1, &err);
		if (err != OPUS_OK) {
			RSS_WARN("media: opus decoder init failed: %d", err);
			bc->opus_dec = NULL;
		}
		c->bc_recv = bc;

		int bc_pt = c->offer.audio_pt > 0 ? c->offer.audio_pt : RWD_AUDIO_PT;
		Compy_BackchannelConfig bc_cfg = {
			.payload_type = (uint8_t)bc_pt,
			.clock_rate = RWD_AUDIO_CLOCK,
		};
		c->backchannel =
			Compy_Backchannel_new(bc_cfg, DYN(rwd_bc_recv_t, Compy_AudioReceiver, bc));

		c->srtp_recv = compy_srtp_recv_new(suite, &recv_key);
		if (c->srtp_recv)
			RSS_DEBUG("media: backchannel ready (pt=%d, opus decoder %s)", bc_pt,
				  bc->opus_dec ? "ok" : "failed");
	}

	c->media_ready = true;
	c->sending = true;
	c->waiting_keyframe = true;

	/* Wake reader threads so they start polling rings immediately */
	pthread_cond_broadcast(&srv->clients_cond);

	/* Request IDR so we get a keyframe quickly (bypass cooldown) */
	int si = c->stream_idx;
	if (si >= 0 && si < RWD_STREAM_COUNT && srv->video_rings[si]) {
		srv->last_idr_us[si] = rss_timestamp_us();
		rss_ring_request_idr(srv->video_rings[si]);
	}

	RSS_DEBUG("media: SRTP stack ready for %s (IDR requested)", c->session_id);
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

	/* Backchannel cleanup.  Compy DYN() is a non-owning trait reference —
	 * Backchannel_drop frees compy's internal state, rwd_bc_recv_t_drop
	 * cleans up the opus decoder, and we free the struct. No double-free. */
	if (c->backchannel) {
		VCALL(DYN(Compy_Backchannel, Compy_Droppable, c->backchannel), drop);
		c->backchannel = NULL;
	}
	free(c->bc_recv);
	c->bc_recv = NULL;
	if (c->srtp_recv) {
		compy_srtp_recv_free(c->srtp_recv);
		c->srtp_recv = NULL;
	}
	if (c->speaker_ring) {
		rss_ring_destroy(c->speaker_ring);
		c->speaker_ring = NULL;
	}
}

/* ── Feed incoming SRTP/SRTCP to backchannel receiver ── */

void rwd_media_feed_rtp(rwd_client_t *c, uint8_t *data, size_t len)
{
	if (!c->srtp_recv || !c->backchannel)
		return;

	Compy_RtpReceiver *recv = Compy_Backchannel_get_receiver(c->backchannel);
	if (!recv)
		return;

	/* Determine RTP vs RTCP by raw byte[1] (RFC 5761):
	 * RTCP types 200-209 don't overlap with RTP dynamic PTs (96-127
	 * without marker, 224-255 with marker). */
	uint8_t ptype = data[1];
	if (ptype >= 200 && ptype <= 209) {
		if (compy_srtcp_recv_unprotect(c->srtp_recv, data, &len) == 0)
			Compy_RtpReceiver_feed(recv, COMPY_CHANNEL_RTCP, data, len);
	} else {
		if (compy_srtp_recv_unprotect(c->srtp_recv, data, &len) == 0)
			Compy_RtpReceiver_feed(recv, COMPY_CHANNEL_RTP, data, len);
	}
}

/* ── Annex B start code scanner (handles both 3-byte and 4-byte) ── */

/* Find next start code (00 00 01 or 00 00 00 01) starting at *pos.
 * Returns pointer to NALU data (after start code), sets *sc_len to
 * start code length (3 or 4). Returns NULL if no start code found. */
static const uint8_t *find_start_code(const uint8_t *p, const uint8_t *end, int *sc_len)
{
	while (p + 3 <= end) {
		if (p[0] == 0 && p[1] == 0) {
			if (p[2] == 1) {
				*sc_len = 3;
				return p + 3;
			}
			if (p + 4 <= end && p[2] == 0 && p[3] == 1) {
				*sc_len = 4;
				return p + 4;
			}
		}
		p++;
	}
	return NULL;
}

/* Find the end of the current NALU (start of next start code, or end of buffer) */
static const uint8_t *find_nalu_end(const uint8_t *nalu_start, const uint8_t *end)
{
	const uint8_t *p = nalu_start;
	while (p + 3 <= end) {
		if (p[0] == 0 && p[1] == 0 &&
		    (p[2] == 1 || (p + 4 <= end && p[2] == 0 && p[3] == 1)))
			return p;
		p++;
	}
	return end;
}

/* ── Parse Annex B and send NALUs via SRTP ── */

static void rwd_send_video_frame(rwd_client_t *c, const uint8_t *data, uint32_t len,
				 uint32_t rtp_ts)
{
	if (!c->nal_video || !c->sending)
		return;

	const uint8_t *end = data + len;

	/* First pass: collect SPS/PPS for STAP-A aggregation */
	const uint8_t *sps_data = NULL, *pps_data = NULL;
	uint32_t sps_len = 0, pps_len = 0;
	const uint8_t *p = data;
	int sc_len;

	while ((p = find_start_code(p, end, &sc_len)) != NULL) {
		const uint8_t *nalu_end = find_nalu_end(p, end);
		uint32_t nl = (uint32_t)(nalu_end - p);
		if (nl >= 1) {
			uint8_t nt = p[0] & 0x1F;
			if (nt == 7) {
				sps_data = p;
				sps_len = nl;
			}
			if (nt == 8) {
				pps_data = p;
				pps_len = nl;
			}
		}
		p = nalu_end;
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
	while ((p = find_start_code(p, end, &sc_len)) != NULL) {
		const uint8_t *nalu_start = p;
		const uint8_t *nalu_end = find_nalu_end(nalu_start, end);
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

static void rwd_send_audio_frame(rwd_client_t *c, const uint8_t *data, uint32_t len,
				 uint32_t rtp_ts)
{
	if (!c->rtp_audio || !c->sending)
		return;

	(void)!Compy_RtpTransport_send_packet(c->rtp_audio, Compy_RtpTimestamp_Raw(rtp_ts), true,
					      U8Slice99_empty(),
					      U8Slice99_new((uint8_t *)data, len));

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

static const char *ring_names[RWD_STREAM_COUNT] = {"main",   "sub",	"s1_main",
						   "s1_sub", "s2_main", "s2_sub"};

void *rwd_video_reader_thread(void *arg)
{
	rwd_server_t *srv = arg;
	int64_t video_ts_epoch[RWD_STREAM_COUNT] = {0};

	/* Wait for at least the main ring */
	while (rss_running(srv->running) && !srv->video_rings[0]) {
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
		{
			char vname[24];
			snprintf(vname, sizeof(vname), "video[%d]", s);
			rss_ring_check_version(srv->video_rings[s], vname);
		}
		const rss_ring_header_t *vhdr = rss_ring_get_header(srv->video_rings[s]);
		srv->video_buf_sizes[s] = rss_ring_max_frame_size(srv->video_rings[s]);
		if (srv->video_buf_sizes[s] < 256 * 1024)
			srv->video_buf_sizes[s] = 256 * 1024;
		srv->video_bufs[s] = malloc(srv->video_buf_sizes[s]);
		if (!srv->video_bufs[s]) {
			RSS_ERROR("media: failed to allocate video buffer[%d]", s);
			continue;
		}
		srv->video_read_seq[s] = vhdr->write_seq;
		srv->video_last_codec[s] = vhdr->codec;
		RSS_DEBUG("media: video reader[%d] started", s);
	}

	uint64_t last_ws[RWD_STREAM_COUNT] = {0};
	int idle[RWD_STREAM_COUNT] = {0};

	while (rss_running(srv->running)) {
		/* Poll all rings (short timeout so we alternate quickly) */
		bool any_polled = false;
		for (int s = 0; s < RWD_STREAM_COUNT; s++) {
			/* Reconnect closed rings */
			if (!srv->video_rings[s] && srv->video_bufs[s]) {
				srv->video_rings[s] = rss_ring_open(ring_names[s]);
				if (srv->video_rings[s]) {
					{
						char vname[24];
						snprintf(vname, sizeof(vname), "video[%d]", s);
						rss_ring_check_version(srv->video_rings[s], vname);
					}
					const rss_ring_header_t *h =
						rss_ring_get_header(srv->video_rings[s]);
					uint32_t ds = rss_ring_max_frame_size(srv->video_rings[s]);
					if (ds < 256 * 1024)
						ds = 256 * 1024;
					if (ds > srv->video_buf_sizes[s]) {
						uint8_t *new_buf = malloc(ds);
						if (!new_buf) {
							rss_ring_close(srv->video_rings[s]);
							srv->video_rings[s] = NULL;
							srv->video_buf_sizes[s] = 0;
							continue;
						}
						free(srv->video_bufs[s]);
						srv->video_bufs[s] = new_buf;
						srv->video_buf_sizes[s] = ds;
					}
					srv->video_read_seq[s] = h->write_seq;
					last_ws[s] = 0;
					idle[s] = 0;
					video_ts_epoch[s] = 0;

					/* Detect codec change — disconnect clients
					 * if codec switched (WebRTC can't renegotiate
					 * mid-session, and H.265 isn't supported). */
					bool codec_changed = (h->codec != srv->video_last_codec[s]);
					srv->video_last_codec[s] = h->codec;
					if (codec_changed) {
						pthread_mutex_lock(&srv->clients_lock);
						for (int i = 0; i < RWD_MAX_CLIENTS; i++) {
							rwd_client_t *c = srv->clients[i];
							if (c && c->sending && c->stream_idx == s) {
								c->sending = false;
								RSS_INFO("media: disconnecting "
									 "WebRTC client on "
									 "stream %d "
									 "(codec changed)",
									 s);
							}
						}
						pthread_mutex_unlock(&srv->clients_lock);
					}

					RSS_INFO("media: video reader[%d] reconnected "
						 "(%s%s)",
						 s, ring_names[s],
						 codec_changed ? ", codec changed" : "");
				}
			}
			if (!srv->video_rings[s] || !srv->video_bufs[s])
				continue;

			/* Skip rings with no active clients — polling an
			 * unwatched ring wastes the timeout and causes the
			 * reader to fall behind on rings that DO have clients. */
			bool has_clients = false;
			pthread_mutex_lock(&srv->clients_lock);
			for (int i = 0; i < RWD_MAX_CLIENTS; i++) {
				if (srv->clients[i] && srv->clients[i]->sending &&
				    srv->clients[i]->stream_idx == s) {
					has_clients = true;
					break;
				}
			}
			pthread_mutex_unlock(&srv->clients_lock);
			if (!has_clients)
				continue;

			any_polled = true;
			int ret = rss_ring_wait(srv->video_rings[s], 50);
			if (ret != 0) {
				const rss_ring_header_t *h =
					rss_ring_get_header(srv->video_rings[s]);
				uint64_t ws = h->write_seq;
				if (ws == last_ws[s])
					idle[s]++;
				else
					idle[s] = 0;
				last_ws[s] = ws;
				if (idle[s] >= 40) { /* ~2s at 50ms timeout */
					RSS_DEBUG("media: video[%d] idle, closing ring (%s)", s,
						  ring_names[s]);
					rss_ring_close(srv->video_rings[s]);
					srv->video_rings[s] = NULL;
					idle[s] = 0;
				}
				continue;
			}
			idle[s] = 0;

			uint32_t length;
			rss_ring_slot_t meta;
			uint64_t read_seq = srv->video_read_seq[s];

			/* Skip to latest when lag exceeds 10 frames.
			 * Small lags (2-3 frames on slower SoCs) are read
			 * sequentially to preserve the reference chain. */
			const rss_ring_header_t *vhdr = rss_ring_get_header(srv->video_rings[s]);
			uint64_t ws = vhdr->write_seq;
			if (ws > read_seq + 10 && read_seq > 0) {
				read_seq = ws - 1;
				rwd_request_idr(srv, s);
				pthread_mutex_lock(&srv->clients_lock);
				for (int i = 0; i < RWD_MAX_CLIENTS; i++) {
					rwd_client_t *c = srv->clients[i];
					if (c && c->sending && c->stream_idx == s)
						c->waiting_keyframe = true;
				}
				pthread_mutex_unlock(&srv->clients_lock);
			}

			ret = rss_ring_read(srv->video_rings[s], &read_seq, srv->video_bufs[s],
					    srv->video_buf_sizes[s], &length, &meta);
			if (ret == RSS_EOVERFLOW) {
				srv->video_read_seq[s] = read_seq;
				rwd_request_idr(srv, s);
				pthread_mutex_lock(&srv->clients_lock);
				for (int i = 0; i < RWD_MAX_CLIENTS; i++) {
					rwd_client_t *c = srv->clients[i];
					if (c && c->sending && c->stream_idx == s)
						c->waiting_keyframe = true;
				}
				pthread_mutex_unlock(&srv->clients_lock);
				continue;
			}
			if (ret != 0)
				continue;

			srv->video_read_seq[s] = read_seq;

			if (video_ts_epoch[s] == 0)
				video_ts_epoch[s] = meta.timestamp;
			uint32_t rtp_ts =
				(uint32_t)((uint64_t)(meta.timestamp - video_ts_epoch[s]) *
					   RWD_VIDEO_CLOCK / 1000000);

			pthread_mutex_lock(&srv->clients_lock);
			for (int i = 0; i < RWD_MAX_CLIENTS; i++) {
				rwd_client_t *c = srv->clients[i];
				if (!c || !c->sending || c->stream_idx != s)
					continue;

				if (c->waiting_keyframe) {
					if (!meta.is_key)
						continue;
					bool initial = !c->video_ts_base_set;
					c->waiting_keyframe = false;
					if (initial) {
						c->video_ts_offset = rtp_ts;
						c->video_ts_base_set = true;
					}
					RSS_DEBUG("media: client[%d] keyframe, %s", s,
						  initial ? "starting" : "resuming");
				}

				uint32_t client_ts = rtp_ts - c->video_ts_offset;
				rwd_send_video_frame(c, srv->video_bufs[s], length, client_ts);
			}
			pthread_mutex_unlock(&srv->clients_lock);
		}

		/* No clients — block on condvar until a client starts sending */
		if (!any_polled) {
			pthread_mutex_lock(&srv->clients_lock);
			struct timespec ts;
			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_sec += 1;
			pthread_cond_timedwait(&srv->clients_cond, &srv->clients_lock, &ts);
			pthread_mutex_unlock(&srv->clients_lock);
		}
	}

	for (int s = 0; s < RWD_STREAM_COUNT; s++) {
		free(srv->video_bufs[s]);
		srv->video_bufs[s] = NULL;
	}
	RSS_DEBUG("media: video reader exiting");
	return NULL;
}

/* ── Audio reader thread ── */

/* Encode a PCM16 sample to μ-law (ITU-T G.711) */
static uint8_t ulaw_encode(int16_t pcm)
{
	int sign = (pcm >> 8) & 0x80;
	if (sign)
		pcm = -pcm;
	if (pcm > 32635)
		pcm = 32635;
	pcm += 0x84;
	int exponent = 7;
	for (int mask = 0x4000; (pcm & mask) == 0 && exponent > 0; exponent--, mask >>= 1)
		;
	int mantissa = (pcm >> (exponent + 3)) & 0x0F;
	return (uint8_t)(~(sign | (exponent << 4) | mantissa));
}

/* Convert L16 (big-endian PCM16) to PCMU (8kHz μ-law).
 * Downsamples 2:1 with 2-tap averaging if source is 16kHz. */
static int rwd_l16_to_pcmu(const uint8_t *data, uint32_t len, uint8_t *out, int out_size,
			   int sample_rate)
{
	const uint16_t *src = (const uint16_t *)data;
	int samples = (int)(len / 2);
	int step = (sample_rate > 8000) ? sample_rate / 8000 : 1;
	samples -= samples % step; /* ensure even division for averaging */
	int out_samples = samples / step;
	if (out_samples > out_size)
		out_samples = out_size;
	if (step == 2) {
		/* 2-tap low-pass: average adjacent samples to reduce aliasing */
		for (int i = 0; i < out_samples; i++) {
			int16_t a = (int16_t)ntohs(src[i * 2]);
			int16_t b = (int16_t)ntohs(src[i * 2 + 1]);
			out[i] = ulaw_encode((int16_t)((a + b) / 2));
		}
	} else {
		for (int i = 0; i < out_samples; i++)
			out[i] = ulaw_encode((int16_t)ntohs(src[i * step]));
	}
	return out_samples;
}

/* Decode one ring frame to PCM. Returns number of PCM samples produced,
 * or 0 on failure. Output is native-endian 16-bit mono. */
static int rwd_decode_to_pcm(uint32_t codec, const uint8_t *data, uint32_t len,
#ifdef RAPTOR_AAC
			     HAACDecoder aac_dec,
#endif
			     int16_t *pcm, int pcm_max)
{
	int pcm_samples = 0;

	switch (codec) {
	case RWD_CODEC_PCMU:
		pcm_samples = (int)len;
		if (pcm_samples > pcm_max)
			pcm_samples = pcm_max;
		for (int i = 0; i < pcm_samples; i++)
			pcm[i] = ulaw_decode(data[i]);
		break;

	case RWD_CODEC_PCMA:
		pcm_samples = (int)len;
		if (pcm_samples > pcm_max)
			pcm_samples = pcm_max;
		for (int i = 0; i < pcm_samples; i++)
			pcm[i] = alaw_decode(data[i]);
		break;

	case RWD_CODEC_L16: {
		const uint16_t *src = (const uint16_t *)data;
		pcm_samples = (int)(len / 2);
		if (pcm_samples > pcm_max)
			pcm_samples = pcm_max;
		for (int i = 0; i < pcm_samples; i++)
			pcm[i] = (int16_t)ntohs(src[i]);
		break;
	}

#ifdef RAPTOR_AAC
	case RWD_CODEC_AAC:
		if (!aac_dec)
			return 0;
		{
			unsigned char *inp = (unsigned char *)data;
			int left = (int)len;
			int err = AACDecode(aac_dec, &inp, &left, pcm);
			if (err != 0)
				return 0;
			AACFrameInfo info;
			AACGetLastFrameInfo(aac_dec, &info);
			pcm_samples = info.outputSamps;
			if (info.nChans == 2) {
				pcm_samples /= 2;
				for (int i = 0; i < pcm_samples; i++)
					pcm[i] = (pcm[i * 2] + pcm[i * 2 + 1]) / 2;
			}
		}
		break;
#endif

	default:
		return 0;
	}

	return pcm_samples;
}

void *rwd_audio_reader_thread(void *arg)
{
	rwd_server_t *srv = arg;

	RSS_DEBUG("media: audio reader started");

	uint32_t audio_codec = 0;
	int sample_rate = 0;
	int64_t audio_ts_epoch = 0;
	uint8_t audio_buf[4096];
	uint8_t transcode_out[1024]; /* max Opus or PCMU frame output */
	uint64_t last_write_seq = 0;
	int idle_count = 0;
	bool need_transcode = false;

	/* PCM accumulation buffer for Opus transcoding (handles non-aligned
	 * frame sizes like AAC's 1024 samples vs Opus's 320/960) */
	int16_t pcm_accum[2048];
	int pcm_accum_fill = 0;
	int opus_frame_size = 0;  /* valid Opus frame size for current sample rate */
	uint32_t opus_rtp_ts = 0; /* running RTP timestamp for Opus output */
	bool opus_rtp_ts_init = false;

	/* Transcoding state (created when needed) */
	OpusEncoder *opus_enc = NULL;
#ifdef RAPTOR_AAC
	HAACDecoder aac_dec = NULL;
#endif

	while (rss_running(srv->running)) {
		if (!srv->audio_ring) {
			srv->audio_ring = rss_ring_open("audio");
			if (!srv->audio_ring) {
				usleep(200000);
				continue;
			}
			rss_ring_check_version(srv->audio_ring, "audio");
			const rss_ring_header_t *ahdr = rss_ring_get_header(srv->audio_ring);
			audio_codec = ahdr->codec;
			sample_rate = ahdr->fps_num;
			srv->audio_read_seq = ahdr->write_seq;
			audio_ts_epoch = 0;
			last_write_seq = 0;
			idle_count = 0;
			pcm_accum_fill = 0;
			opus_rtp_ts = 0;
			opus_rtp_ts_init = false;

			/* Tear down old transcoder if codec changed */
			if (opus_enc) {
				opus_encoder_destroy(opus_enc);
				opus_enc = NULL;
			}
#ifdef RAPTOR_AAC
			if (aac_dec) {
				AACFreeDecoder(aac_dec);
				aac_dec = NULL;
			}
#endif
			need_transcode = false;

			/* Decide wire codec based on audio_mode */
			if (srv->audio_mode == RWD_AUDIO_MODE_AUTO) {
				switch (audio_codec) {
				case RWD_CODEC_OPUS:
					srv->wire_codec = RWD_CODEC_OPUS;
					srv->wire_pt = RWD_AUDIO_PT;
					srv->wire_clock = 48000;
					break;
				case RWD_CODEC_PCMU:
					srv->wire_codec = RWD_CODEC_PCMU;
					srv->wire_pt = 0;
					srv->wire_clock = 8000;
					break;
				case RWD_CODEC_PCMA:
					srv->wire_codec = RWD_CODEC_PCMA;
					srv->wire_pt = 8;
					srv->wire_clock = 8000;
					break;
				case RWD_CODEC_L16:
					/* L16 not supported by WebRTC → encode to PCMU */
					srv->wire_codec = RWD_CODEC_PCMU;
					srv->wire_pt = 0;
					srv->wire_clock = 8000;
					need_transcode = true;
					break;
				default:
					/* AAC and others → transcode to Opus */
					srv->wire_codec = RWD_CODEC_OPUS;
					srv->wire_pt = RWD_AUDIO_PT;
					srv->wire_clock = 48000;
					need_transcode = true;
					break;
				}
			} else {
				/* opus mode: always transcode to Opus */
				srv->wire_codec = RWD_CODEC_OPUS;
				srv->wire_pt = RWD_AUDIO_PT;
				srv->wire_clock = 48000;
				need_transcode = (audio_codec != RWD_CODEC_OPUS);
			}

			if (need_transcode && srv->wire_codec == RWD_CODEC_OPUS) {
				int err;
				opus_enc = opus_encoder_create(sample_rate, 1,
							       OPUS_APPLICATION_AUDIO, &err);
				if (err != OPUS_OK) {
					RSS_WARN("media: opus encoder init failed: %d", err);
					opus_enc = NULL;
				} else {
					opus_encoder_ctl(opus_enc,
							 OPUS_SET_BITRATE(srv->opus_bitrate));
					opus_encoder_ctl(opus_enc,
							 OPUS_SET_COMPLEXITY(srv->opus_complexity));
					opus_frame_size = sample_rate / 50; /* 20ms */
				}
#ifdef RAPTOR_AAC
				if (audio_codec == RWD_CODEC_AAC) {
					aac_dec = AACInitDecoder();
					if (aac_dec) {
						AACFrameInfo fi = {0};
						fi.nChans = 1;
						fi.sampRateCore = sample_rate;
						AACSetRawBlockParams(aac_dec, 0, &fi);
					} else {
						RSS_WARN("media: AAC decoder init failed");
					}
				}
#endif
			}

			/* Set has_audio last — signaling thread reads it to
			 * gate SDP audio, must see wire fields first. */
			srv->has_audio = true;

			const char *codec_name = audio_codec == RWD_CODEC_PCMU	 ? "PCMU"
						 : audio_codec == RWD_CODEC_PCMA ? "PCMA"
						 : audio_codec == RWD_CODEC_L16	 ? "L16"
						 : audio_codec == RWD_CODEC_AAC	 ? "AAC"
						 : audio_codec == RWD_CODEC_OPUS ? "Opus"
										 : "unknown";
			const char *wire_name = srv->wire_codec == RWD_CODEC_PCMU   ? "PCMU"
						: srv->wire_codec == RWD_CODEC_PCMA ? "PCMA"
						: srv->wire_codec == RWD_CODEC_OPUS ? "Opus"
										    : "unknown";
			if (need_transcode)
				RSS_INFO("media: audio %s → %s (%d Hz)", codec_name, wire_name,
					 sample_rate);
			else
				RSS_INFO("media: audio %s passthrough", codec_name);
		}

		/* Skip ring polling when no clients are sending */
		bool has_audio_clients = false;
		pthread_mutex_lock(&srv->clients_lock);
		for (int i = 0; i < RWD_MAX_CLIENTS; i++) {
			if (srv->clients[i] && srv->clients[i]->sending) {
				has_audio_clients = true;
				break;
			}
		}
		if (!has_audio_clients) {
			struct timespec ats;
			clock_gettime(CLOCK_REALTIME, &ats);
			ats.tv_sec += 1;
			pthread_cond_timedwait(&srv->clients_cond, &srv->clients_lock, &ats);
			pthread_mutex_unlock(&srv->clients_lock);
			continue;
		}
		pthread_mutex_unlock(&srv->clients_lock);

		int ret = rss_ring_wait(srv->audio_ring, 100);
		if (ret != 0) {
			const rss_ring_header_t *h = rss_ring_get_header(srv->audio_ring);
			uint64_t ws = h->write_seq;
			if (ws == last_write_seq)
				idle_count++;
			else
				idle_count = 0;
			last_write_seq = ws;

			if (idle_count >= 20) {
				RSS_DEBUG("media: audio ring idle, closing");
				rss_ring_close(srv->audio_ring);
				srv->audio_ring = NULL;
				idle_count = 0;
			}
			continue;
		}
		idle_count = 0;

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

			/* Skip transcoding + send when no clients are active */
			if (srv->client_count == 0) {
				if (pcm_accum_fill > 0) {
					pcm_accum_fill = 0;
					opus_rtp_ts_init = false;
				}
				continue;
			}

			if (audio_ts_epoch == 0)
				audio_ts_epoch = meta.timestamp;
			uint32_t rtp_ts = (uint32_t)((uint64_t)(meta.timestamp - audio_ts_epoch) *
						     srv->wire_clock / 1000000);

			if (!need_transcode) {
				/* Passthrough: send ring data directly */
				pthread_mutex_lock(&srv->clients_lock);
				for (int i = 0; i < RWD_MAX_CLIENTS; i++) {
					rwd_client_t *c = srv->clients[i];
					if (!c || !c->sending || !c->rtp_audio)
						continue;
					if (!c->audio_ts_base_set) {
						c->audio_ts_offset = rtp_ts;
						c->audio_ts_base_set = true;
					}
					rwd_send_audio_frame(c, audio_buf, length,
							     rtp_ts - c->audio_ts_offset);
				}
				pthread_mutex_unlock(&srv->clients_lock);
			} else if (srv->wire_codec == RWD_CODEC_PCMU) {
				/* L16 → PCMU (table lookup) */
				int enc_len = rwd_l16_to_pcmu(audio_buf, length, transcode_out,
							      sizeof(transcode_out), sample_rate);
				if (enc_len > 0) {
					pthread_mutex_lock(&srv->clients_lock);
					for (int i = 0; i < RWD_MAX_CLIENTS; i++) {
						rwd_client_t *c = srv->clients[i];
						if (!c || !c->sending || !c->rtp_audio)
							continue;
						if (!c->audio_ts_base_set) {
							c->audio_ts_offset = rtp_ts;
							c->audio_ts_base_set = true;
						}
						rwd_send_audio_frame(c, transcode_out, enc_len,
								     rtp_ts - c->audio_ts_offset);
					}
					pthread_mutex_unlock(&srv->clients_lock);
				}
			} else if (opus_enc && opus_frame_size > 0) {
				/* Decode → accumulate → encode Opus in 20ms frames.
				 * Each Opus frame advances RTP ts by 960 (48kHz × 20ms).
				 * Decode directly into pcm_accum to avoid a 4KB temp buffer.
				 * Need at least 1024 slots free: AAC decoder writes a full
				 * frame regardless of pcm_max. */
				int space = 2048 - pcm_accum_fill;
				if (space >= 1024) {
					int n = rwd_decode_to_pcm(audio_codec, audio_buf, length,
#ifdef RAPTOR_AAC
								  aac_dec,
#endif
								  pcm_accum + pcm_accum_fill,
								  space);
					if (n > 0)
						pcm_accum_fill += n;
				}
				/* Use running timestamp — avoids gaps between
				 * AAC frame batches (1024 vs 3×320 = 960) */
				if (!opus_rtp_ts_init) {
					opus_rtp_ts = rtp_ts;
					opus_rtp_ts_init = true;
				}
				while (pcm_accum_fill >= opus_frame_size) {
					opus_int32 el =
						opus_encode(opus_enc, pcm_accum, opus_frame_size,
							    transcode_out, sizeof(transcode_out));
					pcm_accum_fill -= opus_frame_size;
					if (pcm_accum_fill > 0)
						memmove(pcm_accum, pcm_accum + opus_frame_size,
							pcm_accum_fill * 2);
					if (el <= 0) {
						opus_rtp_ts += 960;
						continue;
					}
					pthread_mutex_lock(&srv->clients_lock);
					for (int i = 0; i < RWD_MAX_CLIENTS; i++) {
						rwd_client_t *c = srv->clients[i];
						if (!c || !c->sending || !c->rtp_audio)
							continue;
						if (!c->audio_ts_base_set) {
							c->audio_ts_offset = opus_rtp_ts;
							c->audio_ts_base_set = true;
						}
						rwd_send_audio_frame(c, transcode_out, el,
								     opus_rtp_ts -
									     c->audio_ts_offset);
					}
					pthread_mutex_unlock(&srv->clients_lock);
					opus_rtp_ts += 960;
				}
			}
		}
	}

	if (opus_enc)
		opus_encoder_destroy(opus_enc);
#ifdef RAPTOR_AAC
	if (aac_dec)
		AACFreeDecoder(aac_dec);
#endif
	RSS_DEBUG("media: audio reader exiting");
	return NULL;
}

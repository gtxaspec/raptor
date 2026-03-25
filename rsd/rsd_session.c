/*
 * rsd_session.c -- Compy_Controller implementation (RTSP session handling)
 *
 * Each connected client gets an rsd_client_t. This file implements
 * the RTSP method handlers via compy's Controller interface.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>

#include "rsd.h"

/* Server pointer for SDP generation (set by rsd_handle_rtsp_data) */
static rsd_server_t *g_srv = NULL;

/* ── Backchannel audio receiver (separate type for interface99) ── */

typedef struct {
	rss_ring_t **speaker_ring_ptr; /* points to client->speaker_ring */
} rsd_bc_recv_t;

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

static void rsd_bc_recv_t_on_audio(VSelf, uint8_t payload_type, uint32_t timestamp, uint32_t ssrc,
				   U8Slice99 payload)
{
	VSELF(rsd_bc_recv_t);
	(void)timestamp;
	(void)ssrc;

	rss_ring_t **ring_ptr = self->speaker_ring_ptr;
	if (!*ring_ptr) {
		*ring_ptr = rss_ring_create("speaker", 16, 64 * 1024);
		if (!*ring_ptr) {
			RSS_WARN("backchannel: failed to create speaker ring");
			return;
		}
		rss_ring_set_stream_info(*ring_ptr, 0x11, 0, 0, 0, 16000, 1, 0, 0);
		RSS_INFO("backchannel: speaker ring created");
	}

	if (payload_type == 0) {
		/* PCMU/8000 — decode to PCM16 and upsample 8kHz→16kHz.
		 * Simple 2x interpolation: duplicate each sample. */
		int16_t pcm[1920]; /* 960 input samples * 2 */
		int n = (int)payload.len;
		if (n > 960)
			n = 960;
		for (int i = 0; i < n; i++) {
			int16_t s = ulaw_decode(payload.ptr[i]);
			pcm[i * 2] = s;
			pcm[i * 2 + 1] = s;
		}
		rss_ring_publish(*ring_ptr, (const uint8_t *)pcm, n * 4, rss_timestamp_us(), 0, 0);
	} else {
		rss_ring_publish(*ring_ptr, payload.ptr, (uint32_t)payload.len, rss_timestamp_us(),
				 payload_type, 0);
	}
}

static void rsd_bc_recv_t_drop(VSelf)
{
	VSELF(rsd_bc_recv_t);
	(void)self;
}

impl(Compy_AudioReceiver, rsd_bc_recv_t);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-const-variable"
impl(Compy_Droppable, rsd_bc_recv_t);
#pragma GCC diagnostic pop

/* ── Controller method implementations ── */

static void rsd_client_t_options(VSelf, Compy_Context *ctx, const Compy_Request *req)
{
	VSELF(rsd_client_t);
	(void)self;
	(void)req;

	compy_header(ctx, COMPY_HEADER_PUBLIC,
		     "DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, GET_PARAMETER");
	compy_respond_ok(ctx);
}

/* Detect stream index from URI */
static int detect_stream_idx(CharSlice99 uri)
{
	if (CharSlice99_primitive_ends_with(uri, CharSlice99_from_str("/stream1")) ||
	    CharSlice99_primitive_ends_with(uri, CharSlice99_from_str("/sub")))
		return RSD_STREAM_SUB;
	return RSD_STREAM_MAIN;
}

static void rsd_client_t_describe(VSelf, Compy_Context *ctx, const Compy_Request *req)
{
	VSELF(rsd_client_t);

	/* Determine which stream from URI */
	self->stream_idx = detect_stream_idx(req->start_line.uri);

	/* Check if the requested stream's ring exists */
	if (!g_srv || !g_srv->video[self->stream_idx].ring) {
		compy_respond(ctx, COMPY_STATUS_NOT_FOUND, "Stream not available");
		return;
	}

	const rss_ring_header_t *hdr = rss_ring_get_header(g_srv->video[self->stream_idx].ring);

	char sdp[2048] = {0};
	Compy_Writer sdp_w = compy_string_writer(sdp);
	ssize_t ret = 0;

	COMPY_SDP_DESCRIBE(ret, sdp_w, (COMPY_SDP_VERSION, "0"),
			   (COMPY_SDP_ORIGIN, "Raptor 1 1 IN IP4 0.0.0.0"),
			   (COMPY_SDP_SESSION_NAME, "Raptor Live"),
			   (COMPY_SDP_CONNECTION, "IN IP4 0.0.0.0"), (COMPY_SDP_TIME, "0 0"),
			   (COMPY_SDP_ATTR, "tool:Raptor RSS"), (COMPY_SDP_ATTR, "range:npt=now-"));

	/* Store codec for NAL framing in ring reader */
	self->video_codec = hdr->codec;

	if (hdr->codec == 1) {
		/* H.265 / HEVC (RFC 7798) */
		COMPY_SDP_DESCRIBE(
			ret, sdp_w, (COMPY_SDP_MEDIA, "video 0 RTP/AVP %d", RSD_VIDEO_PT),
			(COMPY_SDP_ATTR, "control:video"), (COMPY_SDP_ATTR, "recvonly"),
			(COMPY_SDP_ATTR, "rtpmap:%d H265/%d", RSD_VIDEO_PT, RSD_VIDEO_CLOCK),
			(COMPY_SDP_ATTR, "framerate:%u", hdr->fps_num));
	} else {
		/* H.264 / AVC (RFC 6184) */
		uint8_t profile_idc = hdr->profile ? hdr->profile : 100;
		uint8_t level_idc = hdr->level ? hdr->level : 40;

		COMPY_SDP_DESCRIBE(
			ret, sdp_w, (COMPY_SDP_MEDIA, "video 0 RTP/AVP %d", RSD_VIDEO_PT),
			(COMPY_SDP_ATTR, "control:video"), (COMPY_SDP_ATTR, "recvonly"),
			(COMPY_SDP_ATTR, "rtpmap:%d H264/%d", RSD_VIDEO_PT, RSD_VIDEO_CLOCK),
			(COMPY_SDP_ATTR, "fmtp:%d packetization-mode=1;profile-level-id=%02X00%02X",
			 RSD_VIDEO_PT, profile_idc, level_idc),
			(COMPY_SDP_ATTR, "framerate:%u", hdr->fps_num));
	}

	if (g_srv->has_audio) {
		const rss_ring_header_t *ahdr = rss_ring_get_header(g_srv->ring_audio);
		int apt = (ahdr->codec == RSD_CODEC_PCMU)   ? 0
			  : (ahdr->codec == RSD_CODEC_PCMA) ? 8
							    : RSD_AUDIO_PT_L16;
		int aclk = ahdr->fps_num; /* fps_num holds sample_rate */

		if (apt <= 8) {
			/* Static PT (PCMU=0, PCMA=8) — no rtpmap needed */
			COMPY_SDP_DESCRIBE(ret, sdp_w, (COMPY_SDP_MEDIA, "audio 0 RTP/AVP %d", apt),
					   (COMPY_SDP_ATTR, "control:audio"),
					   (COMPY_SDP_ATTR, "recvonly"));
		} else {
			/* Dynamic PT (L16) — need rtpmap */
			COMPY_SDP_DESCRIBE(ret, sdp_w, (COMPY_SDP_MEDIA, "audio 0 RTP/AVP %d", apt),
					   (COMPY_SDP_ATTR, "control:audio"),
					   (COMPY_SDP_ATTR, "rtpmap:%d L16/%d/1", apt, aclk),
					   (COMPY_SDP_ATTR, "recvonly"));
		}
	}

	/* Backchannel: client sends PCMU to server */
	COMPY_SDP_DESCRIBE(ret, sdp_w, (COMPY_SDP_MEDIA, "audio 0 RTP/AVP 0"),
			   (COMPY_SDP_ATTR, "control:backchannel"),
			   (COMPY_SDP_ATTR, "rtpmap:0 PCMU/8000"), (COMPY_SDP_ATTR, "sendonly"));

	(void)ret;

	compy_header(ctx, COMPY_HEADER_CONTENT_TYPE, "application/sdp");
	compy_body(ctx, CharSlice99_from_str(sdp));
	compy_respond_ok(ctx);
}

static void rsd_client_t_setup(VSelf, Compy_Context *ctx, const Compy_Request *req)
{
	VSELF(rsd_client_t);

	/* Determine stream type from URI */
	bool is_audio = CharSlice99_primitive_ends_with(req->start_line.uri,
							CharSlice99_from_str("/audio"));
	bool is_backchannel = CharSlice99_primitive_ends_with(req->start_line.uri,
							      CharSlice99_from_str("/backchannel"));

	/* Parse Transport header */
	CharSlice99 transport_val;
	if (!Compy_HeaderMap_find(&req->header_map, COMPY_HEADER_TRANSPORT, &transport_val)) {
		compy_respond(ctx, COMPY_STATUS_BAD_REQUEST, "`Transport' not present");
		return;
	}

	Compy_TransportConfig tcfg;
	if (compy_parse_transport(&tcfg, transport_val) == -1) {
		compy_respond(ctx, COMPY_STATUS_BAD_REQUEST, "Malformed `Transport'");
		return;
	}

	Compy_Transport rtp_t, rtcp_t;

	if (tcfg.lower == Compy_LowerTransport_TCP) {
		/* TCP interleaved */
		uint8_t rtp_ch = 0, rtcp_ch = 1;
		ifLet(tcfg.interleaved, Compy_ChannelPair_Some, ch)
		{
			rtp_ch = ch->rtp_channel;
			rtcp_ch = ch->rtcp_channel;
		}

		Compy_Writer conn_writer = Compy_Context_get_writer(ctx);
		rtp_t = compy_transport_tcp(conn_writer, rtp_ch, 0);
		rtcp_t = compy_transport_tcp(conn_writer, rtcp_ch, 0);
		self->is_tcp = true;

		compy_header(ctx, COMPY_HEADER_TRANSPORT,
			     "RTP/AVP/TCP;unicast;interleaved=%" PRIu8 "-%" PRIu8, rtp_ch, rtcp_ch);

		RSS_INFO("client SETUP: video TCP interleaved %u-%u", rtp_ch, rtcp_ch);
	} else {
		/* UDP */
		uint16_t cli_rtp = 0, cli_rtcp = 0;
		bool have_ports = false;
		match(tcfg.client_port)
		{
			of(Compy_PortPair_Some, pp)
			{
				cli_rtp = pp->rtp_port;
				cli_rtcp = pp->rtcp_port;
				have_ports = true;
			}
			otherwise
			{
			}
		}
		if (!have_ports) {
			compy_respond(ctx, COMPY_STATUS_BAD_REQUEST, "`client_port' not found");
			return;
		}

		const struct sockaddr *sa = (const struct sockaddr *)&self->addr;
		const void *client_ip = compy_sockaddr_ip(sa);
		if (!client_ip) {
			compy_respond(ctx, COMPY_STATUS_BAD_REQUEST, "Cannot determine client IP");
			return;
		}

		self->udp_rtp_fd = compy_dgram_socket(sa->sa_family, client_ip, cli_rtp);
		if (self->udp_rtp_fd < 0) {
			compy_respond_internal_error(ctx);
			return;
		}

		self->udp_rtcp_fd = compy_dgram_socket(sa->sa_family, client_ip, cli_rtcp);
		if (self->udp_rtcp_fd < 0) {
			close(self->udp_rtp_fd);
			self->udp_rtp_fd = -1;
			compy_respond_internal_error(ctx);
			return;
		}

		rtp_t = compy_transport_udp(self->udp_rtp_fd);
		rtcp_t = compy_transport_udp(self->udp_rtcp_fd);
		self->is_tcp = false;

		/* Get server-side ports */
		struct sockaddr_storage local;
		socklen_t llen = sizeof(local);
		uint16_t srv_rtp = 0, srv_rtcp = 0;
		if (getsockname(self->udp_rtp_fd, (struct sockaddr *)&local, &llen) == 0) {
			if (local.ss_family == AF_INET6)
				srv_rtp = ntohs(((struct sockaddr_in6 *)&local)->sin6_port);
			else
				srv_rtp = ntohs(((struct sockaddr_in *)&local)->sin_port);
		}
		llen = sizeof(local);
		if (getsockname(self->udp_rtcp_fd, (struct sockaddr *)&local, &llen) == 0) {
			if (local.ss_family == AF_INET6)
				srv_rtcp = ntohs(((struct sockaddr_in6 *)&local)->sin6_port);
			else
				srv_rtcp = ntohs(((struct sockaddr_in *)&local)->sin_port);
		}

		compy_header(ctx, COMPY_HEADER_TRANSPORT,
			     "RTP/AVP/UDP;unicast;client_port=%" PRIu16 "-%" PRIu16
			     ";server_port=%" PRIu16 "-%" PRIu16,
			     cli_rtp, cli_rtcp, srv_rtp, srv_rtcp);

		RSS_INFO("client SETUP: video UDP client=%u-%u server=%u-%u", cli_rtp, cli_rtcp,
			 srv_rtp, srv_rtcp);
	}

	if (is_backchannel) {
		/* Backchannel: receive-only, create Compy_Backchannel */
		Compy_BackchannelConfig bc_cfg = {.payload_type = 0, .clock_rate = 8000};
		rsd_bc_recv_t *recv = calloc(1, sizeof(rsd_bc_recv_t));
		if (!recv) {
			compy_respond_internal_error(ctx);
			return;
		}
		recv->speaker_ring_ptr = &self->speaker_ring;
		self->bc_recv = recv;
		self->backchannel = Compy_Backchannel_new(
			bc_cfg, DYN(rsd_bc_recv_t, Compy_AudioReceiver, recv));
		RSS_INFO("client SETUP: backchannel PCMU/8000");
	} else if (is_audio) {
		/* Determine PT and clock from ring metadata */
		int apt = 0;
		int aclk = 8000;
		if (g_srv && g_srv->ring_audio) {
			const rss_ring_header_t *ahdr = rss_ring_get_header(g_srv->ring_audio);
			apt = (ahdr->codec == RSD_CODEC_PCMU)	? 0
			      : (ahdr->codec == RSD_CODEC_PCMA) ? 8
								: RSD_AUDIO_PT_L16;
			aclk = ahdr->fps_num;
		}
		self->audio.rtp = Compy_RtpTransport_new(rtp_t, apt, aclk);
		self->audio.rtcp = Compy_Rtcp_new(self->audio.rtp, rtcp_t, "raptor@camera");
	} else {
		self->video.rtp = Compy_RtpTransport_new(rtp_t, RSD_VIDEO_PT, RSD_VIDEO_CLOCK);
		self->video.nal = Compy_NalTransport_new(self->video.rtp);
		self->video.rtcp = Compy_Rtcp_new(self->video.rtp, rtcp_t, "raptor@camera");
	}

	/* Generate session ID */
	if (!self->session_id) {
		FILE *f = fopen("/dev/urandom", "r");
		if (f) {
			if (fread(&self->session_id, sizeof(self->session_id), 1, f) != 1)
				self->session_id = (uint64_t)rand();
			fclose(f);
		} else {
			self->session_id = (uint64_t)rand();
		}
	}

	compy_header(ctx, COMPY_HEADER_SESSION, "%" PRIu64 ";timeout=60", self->session_id);
	compy_respond_ok(ctx);
}

static void rsd_client_t_play(VSelf, Compy_Context *ctx, const Compy_Request *req)
{
	VSELF(rsd_client_t);
	(void)req;

	if (!self->video.nal && !self->audio.rtp) {
		compy_respond(ctx, COMPY_STATUS_METHOD_NOT_VALID_IN_THIS_STATE, "SETUP not done");
		return;
	}

	if (self->video.nal) {
		self->video.playing = true;
		self->waiting_keyframe = true;
		self->video_read_seq = 0;
	}
	if (self->audio.rtp)
		self->audio.playing = true;

	/* Request IDR from RVD so the client gets a keyframe ASAP */
	{
		char resp[128];
		rss_ctrl_send_command("/var/run/rss/rvd.sock", "{\"cmd\":\"request-idr\"}", resp,
				      sizeof(resp), 1000);
	}

	/* Build RTP-Info header (RFC 2326 Section 12.33)
	 * Tells the client the initial seq and rtptime for each stream
	 * so it can correctly synchronize A/V from the first packet. */
	{
		char rtp_info[512];
		int off = 0;

		if (self->video.rtp) {
			uint16_t vseq = Compy_RtpTransport_get_seq(self->video.rtp);
			off += snprintf(rtp_info + off, sizeof(rtp_info) - off,
					"url=rtsp://*/video;seq=%u;rtptime=0", vseq);
		}
		if (self->audio.rtp) {
			uint16_t aseq = Compy_RtpTransport_get_seq(self->audio.rtp);
			if (off > 0)
				off += snprintf(rtp_info + off, sizeof(rtp_info) - off, ",");
			off += snprintf(rtp_info + off, sizeof(rtp_info) - off,
					"url=rtsp://*/audio;seq=%u;rtptime=0", aseq);
		}

		if (off > 0)
			compy_header(ctx, COMPY_HEADER_RTP_INFO, "%s", rtp_info);
	}

	compy_header(ctx, COMPY_HEADER_SESSION, "%" PRIu64, self->session_id);
	compy_respond_ok(ctx);

	RSS_INFO("client PLAY (IDR requested)");
}

static void rsd_client_t_pause_method(VSelf, Compy_Context *ctx, const Compy_Request *req)
{
	VSELF(rsd_client_t);
	(void)req;

	self->video.playing = false;
	compy_respond_ok(ctx);
}

static void rsd_client_t_teardown(VSelf, Compy_Context *ctx, const Compy_Request *req)
{
	VSELF(rsd_client_t);
	(void)req;

	self->video.playing = false;

	if (self->video.rtcp)
		(void)!Compy_Rtcp_send_bye(self->video.rtcp);

	compy_respond_ok(ctx);
	RSS_INFO("client TEARDOWN");
}

static void rsd_client_t_get_parameter(VSelf, Compy_Context *ctx, const Compy_Request *req)
{
	VSELF(rsd_client_t);
	(void)self;
	(void)req;

	/* Keep-alive response */
	compy_respond_ok(ctx);
}

static void rsd_client_t_unknown(VSelf, Compy_Context *ctx, const Compy_Request *req)
{
	VSELF(rsd_client_t);
	(void)self;
	(void)req;

	compy_respond(ctx, COMPY_STATUS_NOT_IMPLEMENTED, "Not implemented");
}

static Compy_ControlFlow rsd_client_t_before(VSelf, Compy_Context *ctx, const Compy_Request *req)
{
	VSELF(rsd_client_t);
	(void)self;

	if (g_srv && g_srv->auth) {
		if (compy_auth_check(g_srv->auth, ctx, req) != 0)
			return Compy_ControlFlow_Break;
	}

	return Compy_ControlFlow_Continue;
}

static void rsd_client_t_after(VSelf, ssize_t ret, Compy_Context *ctx, const Compy_Request *req)
{
	VSELF(rsd_client_t);
	(void)self;
	(void)ret;
	(void)ctx;
	(void)req;
}

static void rsd_client_t_drop(VSelf)
{
	VSELF(rsd_client_t);
	(void)self;
	/* Cleanup handled by remove_client in rsd_server.c */
}

/* Register interface implementations (Droppable must come before Controller) */
impl(Compy_Droppable, rsd_client_t);
impl(Compy_Controller, rsd_client_t);

/* ── RTSP request parsing and dispatch ── */

void rsd_handle_rtsp_data(rsd_server_t *srv, rsd_client_t *client, const char *data, size_t len)
{
	g_srv = srv;

	/* Handle TCP interleaved frames ($ + channel + 2-byte length + payload).
	 * These arrive on the same TCP connection as RTSP signaling. */
	while (len >= 4 && data[0] == '$') {
		uint8_t channel = (uint8_t)data[1];
		uint16_t frame_len = ((uint8_t)data[2] << 8) | (uint8_t)data[3];
		if (4 + (size_t)frame_len > len)
			break; /* incomplete frame */

		/* Feed backchannel data */
		if (client->backchannel) {
			Compy_RtpReceiver *recv =
				Compy_Backchannel_get_receiver(client->backchannel);
			if (recv) {
				uint8_t ch_type =
					(channel & 1) ? COMPY_CHANNEL_RTCP : COMPY_CHANNEL_RTP;
				Compy_RtpReceiver_feed(recv, ch_type, (const uint8_t *)data + 4,
						       frame_len);
			}
		}

		size_t consumed = 4 + frame_len;
		data += consumed;
		len -= consumed;
		if (consumed <= client->recv_len) {
			memmove(client->recv_buf, client->recv_buf + consumed,
				client->recv_len - consumed);
			client->recv_len -= consumed;
		} else {
			client->recv_len = 0;
		}
	}

	if (len == 0)
		return;

	/* Try to parse an RTSP request from the buffer */
	Compy_Request req = Compy_Request_uninit();
	CharSlice99 input = CharSlice99_new((char *)data, len);
	Compy_ParseResult result = Compy_Request_parse(&req, input);

	if (Compy_ParseResult_is_complete(result)) {
		Compy_Writer writer = compy_fd_writer(&client->fd);
		Compy_Controller ctrl = DYN(rsd_client_t, Compy_Controller, client);
		compy_dispatch(writer, ctrl, &req);

		/* Extract consumed byte count */
		size_t consumed = len;
		match(result)
		{
			of(Compy_ParseResult_Success, status)
			{
				match(*status)
				{
					of(Compy_ParseStatus_Complete, offset)
					{
						consumed = *offset;
					}
					otherwise
					{
					}
				}
			}
			otherwise
			{
			}
		}

		/* Remove parsed bytes from buffer */
		if (consumed < client->recv_len) {
			memmove(client->recv_buf, client->recv_buf + consumed,
				client->recv_len - consumed);
			client->recv_len -= consumed;
		} else {
			client->recv_len = 0;
		}
	}
	/* If incomplete, data stays in recv_buf for next read */
}

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

		const void *client_ip = compy_sockaddr_ip((const struct sockaddr *)&self->addr);
		if (!client_ip) {
			compy_respond(ctx, COMPY_STATUS_BAD_REQUEST, "Cannot determine client IP");
			return;
		}

		self->udp_rtp_fd = compy_dgram_socket(AF_INET, client_ip, cli_rtp);
		if (self->udp_rtp_fd < 0) {
			compy_respond_internal_error(ctx);
			return;
		}

		self->udp_rtcp_fd = compy_dgram_socket(AF_INET, client_ip, cli_rtcp);
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
		struct sockaddr_in local;
		socklen_t llen = sizeof(local);
		uint16_t srv_rtp = 0, srv_rtcp = 0;
		if (getsockname(self->udp_rtp_fd, (struct sockaddr *)&local, &llen) == 0)
			srv_rtp = ntohs(local.sin_port);
		llen = sizeof(local);
		if (getsockname(self->udp_rtcp_fd, (struct sockaddr *)&local, &llen) == 0)
			srv_rtcp = ntohs(local.sin_port);

		compy_header(ctx, COMPY_HEADER_TRANSPORT,
			     "RTP/AVP/UDP;unicast;client_port=%" PRIu16 "-%" PRIu16
			     ";server_port=%" PRIu16 "-%" PRIu16,
			     cli_rtp, cli_rtcp, srv_rtp, srv_rtcp);

		RSS_INFO("client SETUP: video UDP client=%u-%u server=%u-%u", cli_rtp, cli_rtcp,
			 srv_rtp, srv_rtcp);
	}

	if (is_audio) {
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
	(void)ctx;
	(void)req;

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

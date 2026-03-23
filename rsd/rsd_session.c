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

/* ── Controller method implementations ── */

static void
rsd_client_t_options(VSelf, Compy_Context *ctx, const Compy_Request *req)
{
	VSELF(rsd_client_t);
	(void)self;
	(void)req;

	compy_header(ctx, COMPY_HEADER_PUBLIC,
		     "DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, GET_PARAMETER");
	compy_respond_ok(ctx);
}

static void
rsd_client_t_describe(VSelf, Compy_Context *ctx, const Compy_Request *req)
{
	VSELF(rsd_client_t);
	(void)req;
	(void)self;

	/*
	 * Build SDP. We read stream metadata from a global ring pointer
	 * set by the server. For now we pass it via a file-scope variable.
	 */
	char sdp[2048] = {0};
	Compy_Writer sdp_w = compy_string_writer(sdp);
	ssize_t ret = 0;

	COMPY_SDP_DESCRIBE(ret, sdp_w,
		(COMPY_SDP_VERSION, "0"),
		(COMPY_SDP_ORIGIN, "Raptor 1 1 IN IP4 0.0.0.0"),
		(COMPY_SDP_SESSION_NAME, "Raptor Live"),
		(COMPY_SDP_CONNECTION, "IN IP4 0.0.0.0"),
		(COMPY_SDP_TIME, "0 0"));

	COMPY_SDP_DESCRIBE(ret, sdp_w,
		(COMPY_SDP_MEDIA, "video 0 RTP/AVP %d", RSD_VIDEO_PT),
		(COMPY_SDP_ATTR, "control:video"),
		(COMPY_SDP_ATTR, "recvonly"),
		(COMPY_SDP_ATTR, "rtpmap:%d H264/%d", RSD_VIDEO_PT, RSD_VIDEO_CLOCK),
		(COMPY_SDP_ATTR, "fmtp:%d packetization-mode=1", RSD_VIDEO_PT));

	(void)ret;

	compy_header(ctx, COMPY_HEADER_CONTENT_TYPE, "application/sdp");
	compy_body(ctx, CharSlice99_from_str(sdp));
	compy_respond_ok(ctx);
}

static void
rsd_client_t_setup(VSelf, Compy_Context *ctx, const Compy_Request *req)
{
	VSELF(rsd_client_t);

	/* Parse Transport header */
	CharSlice99 transport_val;
	if (!Compy_HeaderMap_find(&req->header_map, COMPY_HEADER_TRANSPORT,
				  &transport_val)) {
		compy_respond(ctx, COMPY_STATUS_BAD_REQUEST,
			      "`Transport' not present");
		return;
	}

	Compy_TransportConfig tcfg;
	if (compy_parse_transport(&tcfg, transport_val) == -1) {
		compy_respond(ctx, COMPY_STATUS_BAD_REQUEST,
			      "Malformed `Transport'");
		return;
	}

	/* TCP interleaved only for now */
	if (tcfg.lower != Compy_LowerTransport_TCP) {
		compy_respond(ctx, COMPY_STATUS_BAD_REQUEST,
			      "Only TCP interleaved supported");
		return;
	}

	uint8_t rtp_ch = 0, rtcp_ch = 1;
	ifLet(tcfg.interleaved, Compy_ChannelPair_Some, ch) {
		rtp_ch = ch->rtp_channel;
		rtcp_ch = ch->rtcp_channel;
	}

	/* Create transports using the context writer (same TCP connection) */
	Compy_Writer conn_writer = Compy_Context_get_writer(ctx);

	Compy_Transport rtp_t = compy_transport_tcp(conn_writer, rtp_ch, 0);
	Compy_Transport rtcp_t = compy_transport_tcp(conn_writer, rtcp_ch, 0);

	self->video.rtp = Compy_RtpTransport_new(rtp_t, RSD_VIDEO_PT,
						 RSD_VIDEO_CLOCK);
	self->video.nal = Compy_NalTransport_new(self->video.rtp);
	self->video.rtcp = Compy_Rtcp_new(self->video.rtp, rtcp_t,
					  "raptor@camera");

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

	compy_header(ctx, COMPY_HEADER_TRANSPORT,
		     "RTP/AVP/TCP;unicast;interleaved=%" PRIu8 "-%" PRIu8,
		     rtp_ch, rtcp_ch);
	compy_header(ctx, COMPY_HEADER_SESSION,
		     "%" PRIu64 ";timeout=60", self->session_id);
	compy_respond_ok(ctx);

	RSS_INFO("client SETUP: video TCP interleaved %u-%u",
		 rtp_ch, rtcp_ch);
}

static void
rsd_client_t_play(VSelf, Compy_Context *ctx, const Compy_Request *req)
{
	VSELF(rsd_client_t);
	(void)req;

	if (!self->video.nal) {
		compy_respond(ctx, COMPY_STATUS_METHOD_NOT_VALID_IN_THIS_STATE,
			      "SETUP not done");
		return;
	}

	self->video.playing = true;
	self->waiting_keyframe = true;
	self->video_read_seq = 0;

	compy_header(ctx, COMPY_HEADER_SESSION,
		     "%" PRIu64, self->session_id);
	compy_respond_ok(ctx);

	RSS_INFO("client PLAY");
}

static void
rsd_client_t_pause_method(VSelf, Compy_Context *ctx, const Compy_Request *req)
{
	VSELF(rsd_client_t);
	(void)req;

	self->video.playing = false;
	compy_respond_ok(ctx);
}

static void
rsd_client_t_teardown(VSelf, Compy_Context *ctx, const Compy_Request *req)
{
	VSELF(rsd_client_t);
	(void)req;

	self->video.playing = false;

	if (self->video.rtcp)
		(void)!Compy_Rtcp_send_bye(self->video.rtcp);

	compy_respond_ok(ctx);
	RSS_INFO("client TEARDOWN");
}

static void
rsd_client_t_get_parameter(VSelf, Compy_Context *ctx, const Compy_Request *req)
{
	VSELF(rsd_client_t);
	(void)self;
	(void)req;

	/* Keep-alive response */
	compy_respond_ok(ctx);
}

static void
rsd_client_t_unknown(VSelf, Compy_Context *ctx, const Compy_Request *req)
{
	VSELF(rsd_client_t);
	(void)self;
	(void)req;

	compy_respond(ctx, COMPY_STATUS_NOT_IMPLEMENTED, "Not implemented");
}

static Compy_ControlFlow
rsd_client_t_before(VSelf, Compy_Context *ctx, const Compy_Request *req)
{
	VSELF(rsd_client_t);
	(void)self;
	(void)ctx;
	(void)req;

	return Compy_ControlFlow_Continue;
}

static void
rsd_client_t_after(VSelf, ssize_t ret, Compy_Context *ctx,
		const Compy_Request *req)
{
	VSELF(rsd_client_t);
	(void)self;
	(void)ret;
	(void)ctx;
	(void)req;
}

static void
rsd_client_t_drop(VSelf)
{
	VSELF(rsd_client_t);
	(void)self;
	/* Cleanup handled by remove_client in rsd_server.c */
}

/* Register interface implementations (Droppable must come before Controller) */
impl(Compy_Droppable, rsd_client_t);
impl(Compy_Controller, rsd_client_t);

/* ── RTSP request parsing and dispatch ── */

void rsd_handle_rtsp_data(rsd_server_t *srv, rsd_client_t *client,
			  const char *data, size_t len)
{
	(void)srv;

	/* Try to parse an RTSP request from the buffer */
	Compy_Request req = Compy_Request_uninit();
	CharSlice99 input = CharSlice99_new((char *)data, len);
	Compy_ParseResult result = Compy_Request_parse(&req, input);

	if (Compy_ParseResult_is_complete(result)) {
		Compy_Writer writer = compy_fd_writer(&client->fd);
		Compy_Controller ctrl = DYN(rsd_client_t, Compy_Controller,
					    client);
		compy_dispatch(writer, ctrl, &req);

		/* Extract consumed byte count */
		size_t consumed = len;
		match(result) {
			of(Compy_ParseResult_Success, status) {
				match(*status) {
					of(Compy_ParseStatus_Complete, offset) {
						consumed = *offset;
					}
					otherwise {}
				}
			}
			otherwise {}
		}

		/* Remove parsed bytes from buffer */
		if (consumed < client->recv_len) {
			memmove(client->recv_buf,
				client->recv_buf + consumed,
				client->recv_len - consumed);
			client->recv_len -= consumed;
		} else {
			client->recv_len = 0;
		}
	}
	/* If incomplete, data stays in recv_buf for next read */
}

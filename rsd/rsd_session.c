/*
 * rsd_session.c -- Compy_Controller implementation (RTSP session handling)
 *
 * Each connected client gets an rsd_client_t. This file implements
 * the RTSP method handlers via compy's Controller interface.
 */

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <rss_net.h>

#include "rsd.h"

/* Base64 encode for sprop-parameter-sets */
static int b64_encode(const uint8_t *src, int len, char *dst, int dst_size)
{
	static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	int i, o = 0;
	for (i = 0; i + 2 < len && o + 4 < dst_size; i += 3) {
		dst[o++] = t[(src[i] >> 2) & 0x3F];
		dst[o++] = t[((src[i] & 0x3) << 4) | (src[i + 1] >> 4)];
		dst[o++] = t[((src[i + 1] & 0xF) << 2) | (src[i + 2] >> 6)];
		dst[o++] = t[src[i + 2] & 0x3F];
	}
	if (i < len && o + 4 < dst_size) {
		dst[o++] = t[(src[i] >> 2) & 0x3F];
		if (i + 1 < len) {
			dst[o++] = t[((src[i] & 0x3) << 4) | (src[i + 1] >> 4)];
			dst[o++] = t[(src[i + 1] & 0xF) << 2];
		} else {
			dst[o++] = t[(src[i] & 0x3) << 4];
			dst[o++] = '=';
		}
		dst[o++] = '=';
	}
	dst[o] = '\0';
	return o;
}

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
		*ring_ptr = rss_ring_open("speaker");
		if (!*ring_ptr)
			*ring_ptr = rss_ring_create("speaker", 16, 64 * 1024);
		if (!*ring_ptr) {
			RSS_WARN("backchannel: failed to open/create speaker ring");
			return;
		}
		rss_ring_set_stream_info(*ring_ptr, 0x11, 0, 0, 0, 16000, 1, 0, 0);
		RSS_INFO("backchannel: speaker ring ready");
	}

	if (payload_type == 0) {
		/* PCMU/8000 — decode to PCM16 and upsample 8kHz→16kHz.
		 * Simple 2x interpolation: duplicate each sample.
		 * Max 480 input samples (60ms ptime) keeps stack under 2KB. */
		int16_t pcm[960]; /* 480 input samples * 2 */
		int n = (int)payload.len;
		if (n > 480)
			n = 480;
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

/* Check if URI ends with a given path (builds slash-prefixed string) */
static bool uri_ends_with(CharSlice99 uri, const char *path)
{
	if (!path[0])
		return false;
	char buf[72];
	int n = snprintf(buf, sizeof(buf), "/%s", path);
	if (n < 0 || n >= (int)sizeof(buf))
		return false;
	return CharSlice99_primitive_ends_with(uri, CharSlice99_from_str(buf));
}

/* Strip the last path component from URI (e.g. /stream0/audio -> /stream0) */
static CharSlice99 uri_strip_last(CharSlice99 uri)
{
	for (ptrdiff_t i = uri.len - 1; i >= 0; i--) {
		if (uri.ptr[i] == '/') {
			uri.len = i;
			return uri;
		}
	}
	return uri;
}

/* Config keys in stream-index order. Sole authoritative mapping between
 * [rtsp] config names and stream slots — any code that needs to translate
 * between the two should go through this table. */
static const char *const ENDPOINT_KEYS[RSD_STREAM_COUNT] = {
	"endpoint_main",    "endpoint_sub",    "endpoint_s1_main", "endpoint_s1_sub",
	"endpoint_s2_main", "endpoint_s2_sub", "endpoint_jpeg",	   "endpoint_jpeg_sub",
};

/* Allowed alias characters: alphanumerics plus '-', '_', '.'. Restrictive
 * by design — anything more exotic would complicate URI matching without
 * enabling any reasonable identifier. */
static bool alias_chars_ok(const char *s)
{
	for (size_t i = 0; s[i]; i++) {
		if (!isalnum((unsigned char)s[i]) && s[i] != '-' && s[i] != '_' && s[i] != '.')
			return false;
	}
	return true;
}

/* True when the alias has the syntactic shape of a default /streamN path
 * ("stream" followed by decimal digits). Rejecting this shape regardless
 * of the current RSD_STREAM_COUNT prevents a future capacity increase
 * from silently turning a user alias into a shadow of a default path. */
static bool alias_looks_like_default(const char *s)
{
	if (strncmp(s, "stream", 6) != 0)
		return false;
	const char *tail = s + 6;
	if (!*tail)
		return false;
	for (; *tail; tail++) {
		if (*tail < '0' || *tail > '9')
			return false;
	}
	return true;
}

/* Load and validate [rtsp] endpoint aliases into srv->endpoints[]. Any
 * alias that fails a check is cleared (silently disabled at runtime) and
 * logged as a warning, so a misconfigured field never blocks startup but
 * is always visible. Each accepted alias is logged at INFO level. */
void rsd_endpoints_load(rsd_server_t *srv, rss_config_t *cfg)
{
	static const char *const reserved_paths[] = {
		"main", "sub", "jpeg", "jpeg_sub", "audio", "backchannel", ".", "..", NULL,
	};

	for (int s = 0; s < RSD_STREAM_COUNT; s++) {
		const char *raw = rss_config_get_str(cfg, "rtsp", ENDPOINT_KEYS[s], "");

		/* Reject over-long input before rss_strlcpy silently truncates. */
		if (strlen(raw) >= sizeof(srv->endpoints[s])) {
			RSS_WARN("rtsp.%s exceeds %zu chars; disabling alias", ENDPOINT_KEYS[s],
				 sizeof(srv->endpoints[s]) - 1);
			srv->endpoints[s][0] = '\0';
			continue;
		}
		rss_strlcpy(srv->endpoints[s], raw, sizeof(srv->endpoints[s]));

		char *alias = srv->endpoints[s];
		if (!alias[0])
			continue;

		if (!alias_chars_ok(alias)) {
			RSS_WARN("rtsp.%s=\"%s\" has disallowed characters "
				 "(allowed: a-z 0-9 - _ .); disabling alias",
				 ENDPOINT_KEYS[s], alias);
			alias[0] = '\0';
			continue;
		}

		if (alias_looks_like_default(alias)) {
			RSS_WARN("rtsp.%s=\"%s\" conflicts with default /streamN "
				 "naming; disabling alias",
				 ENDPOINT_KEYS[s], alias);
			alias[0] = '\0';
			continue;
		}

		bool reject = false;
		for (int r = 0; reserved_paths[r]; r++) {
			if (strcmp(alias, reserved_paths[r]) == 0) {
				RSS_WARN("rtsp.%s=\"%s\" conflicts with a reserved "
					 "path; disabling alias",
					 ENDPOINT_KEYS[s], alias);
				reject = true;
				break;
			}
		}
		if (reject) {
			alias[0] = '\0';
			continue;
		}

		for (int i = 0; i < s; i++) {
			if (srv->endpoints[i][0] && strcmp(srv->endpoints[i], alias) == 0) {
				RSS_WARN("rtsp.%s=\"%s\" duplicates rtsp.%s; "
					 "disabling alias",
					 ENDPOINT_KEYS[s], alias, ENDPOINT_KEYS[i]);
				reject = true;
				break;
			}
		}
		if (reject) {
			alias[0] = '\0';
			continue;
		}

		RSS_INFO("RTSP: stream %d -> /%s (default /stream%d disabled)", s, alias, s);
	}
}

/* Detect stream index from URI. Returns -1 for unknown endpoints.
 *
 * Per-stream security gate: a configured alias disables the default
 * /streamN path for that stream only. Streams with no alias keep all
 * defaults. Resolution order (first match wins):
 *   1. Custom alias (srv->endpoints[s]).
 *   2. /streamN — only for streams with a blank alias.
 *   3. Legacy /main, /sub, / — only if that stream has a blank alias.
 *   4. Bare /audio, /backchannel → main — only if main has a blank alias.
 */
static int detect_stream_idx(const rsd_server_t *srv, CharSlice99 uri)
{
	CharSlice99 base = uri;
	bool is_subresource = false;
	if (CharSlice99_primitive_ends_with(uri, CharSlice99_from_str("/audio")) ||
	    CharSlice99_primitive_ends_with(uri, CharSlice99_from_str("/backchannel"))) {
		base = uri_strip_last(uri);
		is_subresource = true;
	}

	for (int s = 0; s < RSD_STREAM_COUNT; s++) {
		if (!srv->jpeg_enabled && s >= RSD_STREAM_JPEG)
			continue;
		if (uri_ends_with(base, srv->endpoints[s]))
			return s;
	}

	static char *const default_paths[RSD_STREAM_COUNT] = {
		"/stream0", "/stream1", "/stream2", "/stream3",
		"/stream4", "/stream5", "/jpeg",    "/jpeg_sub",
	};
	for (int s = 0; s < RSD_STREAM_COUNT; s++) {
		if (!srv->jpeg_enabled && s >= RSD_STREAM_JPEG)
			continue;
		if (srv->endpoints[s][0])
			continue;
		if (CharSlice99_primitive_ends_with(base, CharSlice99_from_str(default_paths[s])))
			return s;
	}

	if (!srv->endpoints[RSD_STREAM_SUB][0] &&
	    CharSlice99_primitive_ends_with(base, CharSlice99_from_str("/sub")))
		return RSD_STREAM_SUB;
	if (!srv->endpoints[RSD_STREAM_MAIN][0] &&
	    (CharSlice99_primitive_ends_with(base, CharSlice99_from_str("/main")) ||
	     CharSlice99_primitive_ends_with(base, CharSlice99_from_str("/"))))
		return RSD_STREAM_MAIN;

	if (is_subresource && !srv->endpoints[RSD_STREAM_MAIN][0])
		return RSD_STREAM_MAIN;

	return -1;
}

static void rsd_client_t_describe(VSelf, Compy_Context *ctx, const Compy_Request *req)
{
	VSELF(rsd_client_t);

	/* Determine which stream from URI */
	int idx = detect_stream_idx(self->srv, req->start_line.uri);
	if (idx < 0) {
		compy_respond(ctx, COMPY_STATUS_NOT_FOUND, "Unknown stream endpoint");
		return;
	}
	self->stream_idx = idx;

	/* Use cached stream info from rsd_ring_ctx_t — never dereference the
	 * ring pointer here. The reader thread may close the ring (idle timeout)
	 * between our check and any header read, causing a UAF. */
	rsd_ring_ctx_t *rctx = &self->srv->video[self->stream_idx];
	bool has_video = rctx->last_width > 0;
	if (!has_video && !self->srv->has_audio) {
		compy_respond(ctx, COMPY_STATUS_NOT_FOUND, "Stream not available");
		return;
	}

	char sdp[2048] = {0};
	Compy_Writer sdp_w = compy_string_writer(sdp);
	ssize_t ret = 0;

	/* Derive server IP from the client's connection (same address
	 * the client connected to). Session ID from monotonic clock. */
	char server_ip[INET6_ADDRSTRLEN] = "0.0.0.0";
	{
		struct sockaddr_storage local;
		socklen_t llen = sizeof(local);
		if (getsockname(self->fd, (struct sockaddr *)&local, &llen) == 0)
			rss_addr_str(&local, server_ip, sizeof(server_ip));
	}
	uint64_t sdp_sess_id = (uint64_t)rss_timestamp_us();

	COMPY_SDP_DESCRIBE(ret, sdp_w, (COMPY_SDP_VERSION, "0"),
			   (COMPY_SDP_ORIGIN, "- %llu 1 IN IP4 %s", (unsigned long long)sdp_sess_id,
			    server_ip),
			   (COMPY_SDP_SESSION_NAME, "%s", self->srv->session_name),
			   (COMPY_SDP_TIME, "0 0"), (COMPY_SDP_ATTR, "tool:Raptor RSS"),
			   (COMPY_SDP_ATTR, "type:broadcast"), (COMPY_SDP_ATTR, "control:*"),
			   (COMPY_SDP_ATTR, "range:npt=now-"));

	if (self->srv->session_info[0])
		COMPY_SDP_DESCRIBE(ret, sdp_w, (COMPY_SDP_INFO, "%s", self->srv->session_info));

	if (has_video) {
		self->video_codec = rctx->last_codec;

		if (rctx->last_codec == 2 || rctx->last_codec == 3) {
			/* JPEG / MJPEG (RFC 2435) */
			COMPY_SDP_DESCRIBE(
				ret, sdp_w, (COMPY_SDP_MEDIA, "video 0 RTP/AVP %d", RSD_JPEG_PT),
				(COMPY_SDP_CONNECTION, "IN IP4 0.0.0.0"),
				(COMPY_SDP_BANDWIDTH, "AS:2000"),
				(COMPY_SDP_ATTR, "rtpmap:%d JPEG/%d", RSD_JPEG_PT, RSD_VIDEO_CLOCK),
				(COMPY_SDP_ATTR, "control:video"),
				(COMPY_SDP_ATTR, "framerate:%u", rctx->last_fps_num));
		} else if (rctx->last_codec == 1) {
			/* H.265 / HEVC (RFC 7798) */
			COMPY_SDP_DESCRIBE(ret, sdp_w,
					   (COMPY_SDP_MEDIA, "video 0 RTP/AVP %d", RSD_VIDEO_PT),
					   (COMPY_SDP_CONNECTION, "IN IP4 0.0.0.0"),
					   (COMPY_SDP_BANDWIDTH, "AS:2000"),
					   (COMPY_SDP_ATTR, "rtpmap:%d H265/%d", RSD_VIDEO_PT,
					    RSD_VIDEO_CLOCK),
					   (COMPY_SDP_ATTR, "control:video"),
					   (COMPY_SDP_ATTR, "framerate:%u", rctx->last_fps_num));
		} else {
			/* H.264 / AVC (RFC 6184) */
			uint16_t sps_l = atomic_load_explicit(&rctx->sps_len, memory_order_acquire);
			uint16_t pps_l = atomic_load_explicit(&rctx->pps_len, memory_order_acquire);

			uint8_t profile_idc, constraint_flags, level_idc;
			if (sps_l >= 4) {
				profile_idc = rctx->sps[1];
				constraint_flags = rctx->sps[2];
				level_idc = rctx->sps[3];
			} else {
				profile_idc = rctx->last_profile ? rctx->last_profile : 100;
				constraint_flags = 0;
				level_idc = rctx->last_level ? rctx->last_level : 40;
			}

			char fmtp[1024];
			int foff = snprintf(
				fmtp, sizeof(fmtp),
				"fmtp:%d packetization-mode=1;profile-level-id=%02X%02X%02X",
				RSD_VIDEO_PT, profile_idc, constraint_flags, level_idc);
			if (foff < 0 || foff >= (int)sizeof(fmtp))
				foff = (int)sizeof(fmtp) - 1;
			if (sps_l > 0 && pps_l > 0) {
				char sps_b64[512], pps_b64[128];
				b64_encode(rctx->sps, sps_l, sps_b64, sizeof(sps_b64));
				b64_encode(rctx->pps, pps_l, pps_b64, sizeof(pps_b64));
				snprintf(fmtp + foff, sizeof(fmtp) - foff,
					 ";sprop-parameter-sets=%s,%s", sps_b64, pps_b64);
			}

			COMPY_SDP_DESCRIBE(
				ret, sdp_w, (COMPY_SDP_MEDIA, "video 0 RTP/AVP %d", RSD_VIDEO_PT),
				(COMPY_SDP_CONNECTION, "IN IP4 0.0.0.0"),
				(COMPY_SDP_BANDWIDTH, "AS:2000"),
				(COMPY_SDP_ATTR, "rtpmap:%d H264/%d", RSD_VIDEO_PT,
				 RSD_VIDEO_CLOCK),
				(COMPY_SDP_ATTR, "%s", fmtp), (COMPY_SDP_ATTR, "control:video"),
				(COMPY_SDP_ATTR, "framerate:%u", rctx->last_fps_num));
		}
	}

	if (self->srv->has_audio) {
		const rss_ring_header_t *ahdr = rss_ring_get_header(self->srv->ring_audio);
		uint32_t codec = ahdr->codec;
		int aclk = ahdr->fps_num; /* fps_num holds sample_rate */

		if (codec == RSD_CODEC_PCMU) {
			COMPY_SDP_DESCRIBE(ret, sdp_w, (COMPY_SDP_MEDIA, "audio 0 RTP/AVP 0"),
					   (COMPY_SDP_CONNECTION, "IN IP4 0.0.0.0"),
					   (COMPY_SDP_BANDWIDTH, "AS:64"),
					   (COMPY_SDP_ATTR, "control:audio"));
		} else if (codec == RSD_CODEC_PCMA) {
			COMPY_SDP_DESCRIBE(ret, sdp_w, (COMPY_SDP_MEDIA, "audio 0 RTP/AVP 8"),
					   (COMPY_SDP_CONNECTION, "IN IP4 0.0.0.0"),
					   (COMPY_SDP_BANDWIDTH, "AS:64"),
					   (COMPY_SDP_ATTR, "control:audio"));
		} else if (codec == RSD_CODEC_AAC) {
			/* AAC-LC (RFC 3640, AAC-hbr mode) */
			static const int sr_table[] = {96000, 88200, 64000, 48000, 44100,
						       32000, 24000, 22050, 16000, 12000,
						       11025, 8000,  7350};
			int sr_idx = 4;
			for (int i = 0; i < 13; i++) {
				if (sr_table[i] == aclk) {
					sr_idx = i;
					break;
				}
			}
			uint16_t asc = (uint16_t)((2 << 11) | (sr_idx << 7) | (1 << 3));

			COMPY_SDP_DESCRIBE(
				ret, sdp_w,
				(COMPY_SDP_MEDIA, "audio 0 RTP/AVP %d", RSD_AUDIO_PT_AAC),
				(COMPY_SDP_CONNECTION, "IN IP4 0.0.0.0"),
				(COMPY_SDP_BANDWIDTH, "AS:64"),
				(COMPY_SDP_ATTR, "rtpmap:%d mpeg4-generic/%d/1", RSD_AUDIO_PT_AAC,
				 aclk),
				(COMPY_SDP_ATTR,
				 "fmtp:%d streamtype=5;profile-level-id=1;mode=AAC-hbr;"
				 "sizelength=13;indexlength=3;indexdeltalength=3;config=%04X",
				 RSD_AUDIO_PT_AAC, asc),
				(COMPY_SDP_ATTR, "control:audio"));
		} else if (codec == RSD_CODEC_OPUS) {
			COMPY_SDP_DESCRIBE(
				ret, sdp_w,
				(COMPY_SDP_MEDIA, "audio 0 RTP/AVP %d", RSD_AUDIO_PT_OPUS),
				(COMPY_SDP_CONNECTION, "IN IP4 0.0.0.0"),
				(COMPY_SDP_BANDWIDTH, "AS:64"),
				(COMPY_SDP_ATTR, "rtpmap:%d opus/48000/2", RSD_AUDIO_PT_OPUS),
				(COMPY_SDP_ATTR, "control:audio"));
		} else {
			COMPY_SDP_DESCRIBE(
				ret, sdp_w,
				(COMPY_SDP_MEDIA, "audio 0 RTP/AVP %d", RSD_AUDIO_PT_L16),
				(COMPY_SDP_CONNECTION, "IN IP4 0.0.0.0"),
				(COMPY_SDP_BANDWIDTH, "AS:256"),
				(COMPY_SDP_ATTR, "rtpmap:%d L16/%d/1", RSD_AUDIO_PT_L16, aclk),
				(COMPY_SDP_ATTR, "control:audio"));
		}
	}

	/* Backchannel: advertise when client requests it via Require header
	 * (ONVIF Profile T) AND config allows it. Disable with
	 * [rtsp] backchannel = false. */
	if (rss_config_get_bool(self->srv->cfg, "rtsp", "backchannel", false) &&
	    compy_require_has_tag(&req->header_map,
				  CharSlice99_from_str("www.onvif.org/ver20/backchannel"))) {
		COMPY_SDP_DESCRIBE(ret, sdp_w, (COMPY_SDP_MEDIA, "audio 0 RTP/AVP 0"),
				   (COMPY_SDP_ATTR, "control:backchannel"),
				   (COMPY_SDP_ATTR, "rtpmap:0 PCMU/8000"),
				   (COMPY_SDP_ATTR, "sendonly"));
	}

	(void)ret;

	/* Content-Base: tells clients how to resolve relative control URLs
	 * (RFC 2326 Section 12.5). Must end with '/'. */
	compy_header(ctx, COMPY_HEADER_CONTENT_BASE, "%.*s/", (int)req->start_line.uri.len,
		     req->start_line.uri.ptr);
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

		RSS_DEBUG("client SETUP: %s TCP interleaved %u-%u",
			  is_backchannel ? "backchannel"
			  : is_audio	 ? "audio"
					 : "video",
			  rtp_ch, rtcp_ch);

		/* Track RTCP channel for incoming RR routing */
		if (!is_backchannel) {
			if (is_audio)
				self->audio_rtcp_ch = rtcp_ch;
			else
				self->video_rtcp_ch = rtcp_ch;
		}
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

		RSS_DEBUG("client SETUP: video UDP client=%u-%u server=%u-%u", cli_rtp, cli_rtcp,
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
		if (!self->srv->has_audio) {
			compy_respond(ctx, COMPY_STATUS_NOT_FOUND, "Audio not available");
			return;
		}
		/* Determine PT and clock from ring metadata */
		int apt = 0;
		int aclk = 8000;
		if (self->srv->ring_audio) {
			const rss_ring_header_t *ahdr = rss_ring_get_header(self->srv->ring_audio);
			aclk = ahdr->fps_num;
			switch (ahdr->codec) {
			case RSD_CODEC_PCMU:
				apt = 0;
				break;
			case RSD_CODEC_PCMA:
				apt = 8;
				break;
			case RSD_CODEC_AAC:
				apt = RSD_AUDIO_PT_AAC;
				break;
			case RSD_CODEC_OPUS:
				apt = RSD_AUDIO_PT_OPUS;
				aclk = 48000; /* RFC 7587: always 48kHz RTP clock */
				break;
			default:
				apt = RSD_AUDIO_PT_L16;
				break;
			}
		}
		self->audio.rtp = Compy_RtpTransport_new(rtp_t, apt, aclk);
		self->audio.rtcp = Compy_Rtcp_new(self->audio.rtp, rtcp_t, "raptor@camera");
	} else {
		/* Video SETUP — reject if this stream has no video */
		if (!self->srv->video[self->stream_idx].last_width) {
			compy_respond(ctx, COMPY_STATUS_NOT_FOUND, "Video not available");
			return;
		}
		if (self->video_codec == 2 || self->video_codec == 3) {
			self->video.rtp =
				Compy_RtpTransport_new(rtp_t, RSD_JPEG_PT, RSD_VIDEO_CLOCK);
			self->video.jpeg = Compy_JpegTransport_new(self->video.rtp);
		} else {
			self->video.rtp =
				Compy_RtpTransport_new(rtp_t, RSD_VIDEO_PT, RSD_VIDEO_CLOCK);
			self->video.nal = Compy_NalTransport_new(self->video.rtp);
		}
		self->video.rtcp = Compy_Rtcp_new(self->video.rtp, rtcp_t, "raptor@camera");
	}

	/* Generate session ID */
	if (!self->session_id) {
		FILE *f = fopen("/dev/urandom", "r");
		if (f) {
			if (fread(&self->session_id, sizeof(self->session_id), 1, f) != 1)
				self->session_id = 0;
			fclose(f);
		}
		if (!self->session_id) {
			compy_respond(ctx, COMPY_STATUS_INTERNAL_SERVER_ERROR, "RNG unavailable");
			return;
		}
	}

	compy_header(ctx, COMPY_HEADER_SESSION, "%" PRIu64 ";timeout=%d", self->session_id,
		     self->srv->session_timeout);
	compy_respond_ok(ctx);
}

static void rsd_client_t_play(VSelf, Compy_Context *ctx, const Compy_Request *req)
{
	VSELF(rsd_client_t);
	(void)req;

	if (!self->video.nal && !self->video.jpeg && !self->audio.rtp) {
		compy_respond(ctx, COMPY_STATUS_METHOD_NOT_VALID_IN_THIS_STATE, "SETUP not done");
		return;
	}

	/* Build and send the PLAY response BEFORE enabling playback.
	 * This ensures the RTP-Info header is on the TCP connection
	 * before any RTP data — otherwise the client can't calibrate
	 * timestamps and reports "No video PTS." */

	/* Build RTP-Info header (RFC 2326 Section 12.33).
	 * Generate random initial RTP timestamps (RFC 3550 §5.1) so clients
	 * can distinguish "timestamp=0" from "no timestamp".
	 * Strip userinfo (credentials) from the URI — ffmpeg strips them
	 * internally and fails to match if they're present in RTP-Info. */
	{
		char rtp_info[512];
		int off = 0;

		/* Strip trailing '/' from base URI */
		CharSlice99 base = req->start_line.uri;
		while (base.len > 0 && base.ptr[base.len - 1] == '/')
			base.len--;

		/* Strip userinfo: rtsp://user:pass@host → rtsp://host */
		const char *at = NULL;
		for (size_t i = 0; i < base.len; i++) {
			if (base.ptr[i] == '@') {
				at = base.ptr + i;
				break;
			}
			if (base.ptr[i] == '/')
				break;
		}
		char clean_url[512];
		if (at) {
			const char *scheme_end = NULL;
			for (size_t i = 0; i + 2 < base.len; i++) {
				if (base.ptr[i] == '/' && base.ptr[i + 1] == '/') {
					scheme_end = base.ptr + i + 2;
					break;
				}
			}
			if (scheme_end && at > scheme_end) {
				int slen = (int)(scheme_end - base.ptr);
				int rlen = (int)(base.len - (at + 1 - base.ptr));
				snprintf(clean_url, sizeof(clean_url), "%.*s%.*s", slen, base.ptr,
					 rlen, at + 1);
				base = CharSlice99_from_str(clean_url);
			}
		}

		if (self->video.rtp) {
			self->video_ts_rand = (uint32_t)rand();
			uint16_t vseq = Compy_RtpTransport_get_seq(self->video.rtp);
			off += snprintf(rtp_info + off, sizeof(rtp_info) - off,
					"url=%.*s/video;seq=%u;rtptime=%u", (int)base.len, base.ptr,
					vseq, self->video_ts_rand);
		}
		if (self->audio.rtp) {
			self->audio_ts_rand = (uint32_t)rand();
			uint16_t aseq = Compy_RtpTransport_get_seq(self->audio.rtp);
			if (off > 0)
				off += snprintf(rtp_info + off, sizeof(rtp_info) - off, ",");
			off += snprintf(rtp_info + off, sizeof(rtp_info) - off,
					"url=%.*s/audio;seq=%u;rtptime=%u", (int)base.len, base.ptr,
					aseq, self->audio_ts_rand);
		}

		if (off > 0)
			compy_header(ctx, COMPY_HEADER_RTP_INFO, "%s", rtp_info);
	}

	/* Range for live stream — use npt=0.000- (same as live555) so
	 * clients compute stream position from the RTP-Info rtptime. */
	compy_header(ctx, COMPY_HEADER_RANGE, "npt=0.000-");
	compy_header(ctx, COMPY_HEADER_SESSION, "%" PRIu64, self->session_id);
	compy_respond_ok(ctx);

	/* Defer flag-setting until after compy_dispatch returns and
	 * write_lock is released — avoids lock-order inversion with
	 * clients_lock (reader threads hold clients_lock → write_lock). */
	self->play_pending = true;

	RSS_DEBUG("client PLAY%s (pending)",
		  (self->video.nal || self->video.jpeg) ? "" : " (audio only)");
}

static void rsd_client_t_pause_method(VSelf, Compy_Context *ctx, const Compy_Request *req)
{
	VSELF(rsd_client_t);
	(void)req;

	self->video.playing = false;
	self->audio.playing = false;
	compy_respond_ok(ctx);
}

static void rsd_client_t_teardown(VSelf, Compy_Context *ctx, const Compy_Request *req)
{
	VSELF(rsd_client_t);
	(void)req;

	self->video.playing = false;
	self->audio.playing = false;

	if (self->video.rtcp)
		(void)!Compy_Rtcp_send_bye(self->video.rtcp);
	if (self->audio.rtcp)
		(void)!Compy_Rtcp_send_bye(self->audio.rtcp);

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

	if (self->srv->auth) {
		if (compy_auth_check(self->srv->auth, ctx, req) != 0)
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

void rsd_handle_rtsp_data(rsd_client_t *client, const char *data, size_t len)
{
	/* Handle TCP interleaved frames ($ + channel + 2-byte length + payload).
	 * These arrive on the same TCP connection as RTSP signaling.
	 * Advance data/len through all complete frames, then shift recv_buf once. */
	const char *interleaved_start = data;
	while (len >= 4 && data[0] == '$') {
		uint8_t channel = (uint8_t)data[1];
		uint16_t frame_len = ((uint8_t)data[2] << 8) | (uint8_t)data[3];
		if (4 + (size_t)frame_len > len)
			break; /* incomplete frame */

		/* Route incoming RTCP to the correct handler for RR parsing */
		if (channel == client->video_rtcp_ch && client->video.rtcp)
			Compy_Rtcp_handle_incoming(client->video.rtcp, (const uint8_t *)data + 4,
						   frame_len);
		else if (channel == client->audio_rtcp_ch && client->audio.rtcp)
			Compy_Rtcp_handle_incoming(client->audio.rtcp, (const uint8_t *)data + 4,
						   frame_len);

		/* Backchannel data */
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
	}
	/* Shift recv_buf once for all consumed interleaved frames */
	size_t total_consumed = (size_t)(data - interleaved_start);
	if (total_consumed > 0) {
		if (total_consumed <= client->recv_len) {
			memmove(client->recv_buf, client->recv_buf + total_consumed,
				client->recv_len - total_consumed);
			client->recv_len -= total_consumed;
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
		Compy_Writer writer;
#ifdef COMPY_HAS_TLS
		if (client->tls)
			writer = compy_tls_writer(client->tls);
		else
#endif
			writer = compy_fd_writer(&client->fd);
		Compy_Controller ctrl = DYN(rsd_client_t, Compy_Controller, client);
		pthread_mutex_lock(&client->write_lock);
		compy_dispatch(writer, ctrl, &req);
		pthread_mutex_unlock(&client->write_lock);

		/* Apply deferred PLAY — now that write_lock is released,
		 * we can safely take clients_lock (matching the lock order
		 * used by reader threads: clients_lock → write_lock). */
		if (client->play_pending) {
			client->play_pending = false;
			pthread_mutex_lock(&client->srv->clients_lock);
			if (client->video.nal || client->video.jpeg) {
				client->waiting_keyframe = (client->video.jpeg == NULL);
				client->video_ts_base_set = false;
				client->audio_ts_base_set = false;
				atomic_store(&client->video.playing, true);
				client->video.last_rtcp = rss_timestamp_us();
				client->video_read_seq = 0;
			}
			if (client->audio.rtp) {
				atomic_store(&client->audio.playing, true);
				client->audio.last_rtcp = rss_timestamp_us();
			}
			pthread_mutex_unlock(&client->srv->clients_lock);

			if (client->video.nal) {
				char resp[128];
				rss_ctrl_send_command(RSS_RUN_DIR "/rvd.sock",
						      "{\"cmd\":\"request-idr\"}", resp,
						      sizeof(resp), 1000);
				RSS_DEBUG("client PLAY (IDR requested)");
			}
		}

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

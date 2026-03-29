/*
 * rwd_sdp.c -- SDP offer parsing and answer generation
 *
 * Parses the browser's SDP offer to extract ICE credentials, DTLS
 * fingerprint, and codec payload types. Generates a minimal SDP
 * answer with ICE-lite candidate and sendonly media.
 *
 * Only supports H.264 video + Opus audio. No transcoding.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include "rwd.h"

/* ── Line-by-line SDP parser helpers ── */

/* Get next SDP line. Returns pointer to start, sets *next to line after \r\n.
 * Returns NULL when no more lines. */
static const char *sdp_next_line(const char *p, const char **next, char *line_buf, size_t buf_size)
{
	if (!p || !*p)
		return NULL;

	const char *eol = strstr(p, "\r\n");
	if (!eol)
		eol = p + strlen(p);

	size_t len = (size_t)(eol - p);
	if (len >= buf_size)
		len = buf_size - 1;
	memcpy(line_buf, p, len);
	line_buf[len] = '\0';

	*next = *eol ? eol + 2 : eol;
	return line_buf;
}

/* Check if SDP attribute line (a=...) matches a key, return value pointer */
static const char *sdp_attr_value(const char *line, const char *key)
{
	if (line[0] != 'a' || line[1] != '=')
		return NULL;
	const char *attr = line + 2;
	size_t klen = strlen(key);
	if (strncmp(attr, key, klen) != 0)
		return NULL;
	if (attr[klen] == ':')
		return attr + klen + 1;
	if (attr[klen] == '\0')
		return attr + klen;
	return NULL;
}

/* ── Parse SDP offer from browser ── */

int rwd_sdp_parse_offer(const char *sdp, rwd_sdp_offer_t *offer)
{
	memset(offer, 0, sizeof(*offer));
	offer->video_pt = -1;
	offer->audio_pt = -1;
	offer->mid_ext_id = -1;

	char line[512];
	const char *p = sdp;
	const char *next;
	bool in_video = false;
	bool in_audio = false;

	while (sdp_next_line(p, &next, line, sizeof(line))) {
		p = next;

		/* m= line: detect media section */
		if (strncmp(line, "m=video", 7) == 0) {
			in_video = true;
			in_audio = false;
			offer->has_video = true;
			continue;
		}
		if (strncmp(line, "m=audio", 7) == 0) {
			in_video = false;
			in_audio = true;
			offer->has_audio = true;
			continue;
		}
		if (line[0] == 'm') {
			in_video = false;
			in_audio = false;
			continue;
		}

		const char *val;

		/* ICE credentials (take from first occurrence) */
		val = sdp_attr_value(line, "ice-ufrag");
		if (val && !offer->ice_ufrag[0]) {
			rss_strlcpy(offer->ice_ufrag, val, sizeof(offer->ice_ufrag));
			continue;
		}

		val = sdp_attr_value(line, "ice-pwd");
		if (val && !offer->ice_pwd[0]) {
			rss_strlcpy(offer->ice_pwd, val, sizeof(offer->ice_pwd));
			continue;
		}

		/* DTLS fingerprint */
		val = sdp_attr_value(line, "fingerprint");
		if (val && !offer->fingerprint[0]) {
			rss_strlcpy(offer->fingerprint, val, sizeof(offer->fingerprint));
			continue;
		}

		/* DTLS setup role */
		val = sdp_attr_value(line, "setup");
		if (val && !offer->setup[0]) {
			rss_strlcpy(offer->setup, val, sizeof(offer->setup));
			continue;
		}

		/* MID */
		val = sdp_attr_value(line, "mid");
		if (val) {
			if (in_video && !offer->mid_video[0])
				rss_strlcpy(offer->mid_video, val, sizeof(offer->mid_video));
			else if (in_audio && !offer->mid_audio[0])
				rss_strlcpy(offer->mid_audio, val, sizeof(offer->mid_audio));
			continue;
		}

		/* rtpmap: find H264 and opus payload types */
		val = sdp_attr_value(line, "rtpmap");
		if (val) {
			int pt;
			char codec_name[32];
			if (sscanf(val, "%d %31s", &pt, codec_name) == 2) {
				if (pt < 0 || pt > 127)
					continue;
				if (in_video && strncasecmp(codec_name, "H264/", 5) == 0 &&
				    offer->video_pt < 0)
					offer->video_pt = pt;
				if (in_audio && strncasecmp(codec_name, "opus/", 5) == 0 &&
				    offer->audio_pt < 0)
					offer->audio_pt = pt;
			}
			continue;
		}

		/* fmtp: capture for matched video PT */
		val = sdp_attr_value(line, "fmtp");
		if (val && in_video && offer->video_pt >= 0 && !offer->video_fmtp[0]) {
			int pt;
			if (sscanf(val, "%d ", &pt) == 1 && pt == offer->video_pt) {
				/* Skip "PT " prefix, store the parameters */
				const char *params = strchr(val, ' ');
				if (params)
					rss_strlcpy(offer->video_fmtp, params + 1,
						    sizeof(offer->video_fmtp));
			}
			continue;
		}

		/* extmap: find sdes:mid extension ID */
		val = sdp_attr_value(line, "extmap");
		if (val && offer->mid_ext_id < 0) {
			int eid;
			char ext_uri[128];
			if (sscanf(val, "%d %127s", &eid, ext_uri) == 2) {
				if (strstr(ext_uri, "sdes:mid") && eid >= 1 && eid <= 14)
					offer->mid_ext_id = eid;
			}
			continue;
		}
	}

	if (!offer->ice_ufrag[0] || !offer->ice_pwd[0]) {
		RSS_WARN("SDP: missing ICE credentials");
		return -1;
	}
	if (!offer->fingerprint[0]) {
		RSS_WARN("SDP: missing DTLS fingerprint");
		return -1;
	}
	if (!offer->has_video || offer->video_pt < 0) {
		RSS_WARN("SDP: no H264 video offered");
		return -1;
	}

	RSS_DEBUG("SDP offer: ufrag=%s video_pt=%d audio_pt=%d mid_v=%s mid_a=%s", offer->ice_ufrag,
		  offer->video_pt, offer->audio_pt, offer->mid_video, offer->mid_audio);
	return 0;
}

/* ── Generate SDP answer ── */

int rwd_sdp_generate_answer(rwd_client_t *c, const rwd_server_t *srv, char *buf, size_t buf_size)
{
	int off = 0;
	int ret;

	(void)srv; /* video_ring not needed — we echo the browser's fmtp */

	uint32_t session_id = (uint32_t)rss_timestamp_us();

#define APPEND(fmt, ...)                                                                           \
	do {                                                                                       \
		ret = snprintf(buf + off, buf_size - off, fmt "\r\n", ##__VA_ARGS__);              \
		if (ret < 0 || (size_t)ret >= buf_size - off)                                      \
			return -1;                                                                 \
		off += ret;                                                                        \
	} while (0)

	/* Session description */
	APPEND("v=0");
	APPEND("o=- %u 1 IN IP4 %s", session_id, srv->local_ip);
	APPEND("s=Raptor");
	APPEND("t=0 0");

	/* Session-level attributes */
	APPEND("a=ice-lite");
	if (c->offer.has_audio && c->offer.audio_pt >= 0)
		APPEND("a=group:BUNDLE %s %s", c->offer.mid_video[0] ? c->offer.mid_video : "0",
		       c->offer.mid_audio[0] ? c->offer.mid_audio : "1");
	else
		APPEND("a=group:BUNDLE %s", c->offer.mid_video[0] ? c->offer.mid_video : "0");

	/* Video m-line */
	APPEND("m=video %d UDP/TLS/RTP/SAVPF %d", srv->udp_port, c->offer.video_pt);
	APPEND("c=IN IP4 %s", srv->local_ip);
	APPEND("a=rtcp-mux");
	APPEND("a=rtcp-rsize");
	APPEND("a=ice-ufrag:%s", c->local_ufrag);
	APPEND("a=ice-pwd:%s", c->local_pwd);
	APPEND("a=fingerprint:%s", srv->dtls->fingerprint);
	APPEND("a=setup:passive");
	APPEND("a=mid:%s", c->offer.mid_video[0] ? c->offer.mid_video : "0");
	APPEND("a=sendonly");
	/* NOTE: sdes:mid extmap intentionally omitted — pion requires
	 * HandleUndeclaredSSRCWithoutAnswer for PT-based track matching.
	 * go2rtc sets this internally for webrtc: sources. */
	APPEND("a=rtpmap:%d H264/90000", c->offer.video_pt);
	if (c->offer.video_fmtp[0])
		APPEND("a=fmtp:%d %s", c->offer.video_pt, c->offer.video_fmtp);
	else
		APPEND("a=fmtp:%d "
		       "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f",
		       c->offer.video_pt);
	APPEND("a=rtcp-fb:%d nack", c->offer.video_pt);
	APPEND("a=rtcp-fb:%d nack pli", c->offer.video_pt);
	APPEND("a=rtcp-fb:%d ccm fir", c->offer.video_pt);
	/* SSRC declared for pion/go2rtc compatibility. htonl because compy
	 * writes SSRC in host byte order but pion reads as network byte order. */
	APPEND("a=ssrc:%u cname:raptor", (unsigned)htonl(c->video_ssrc));
	APPEND("a=candidate:1 1 UDP 2130706431 %s %d typ host", srv->local_ip, srv->udp_port);

	/* Audio m-line (if browser offered Opus) */
	if (c->offer.has_audio && c->offer.audio_pt >= 0) {
		APPEND("m=audio %d UDP/TLS/RTP/SAVPF %d", srv->udp_port, c->offer.audio_pt);
		APPEND("c=IN IP4 %s", srv->local_ip);
		APPEND("a=rtcp-mux");
		APPEND("a=rtcp-rsize");
		APPEND("a=ice-ufrag:%s", c->local_ufrag);
		APPEND("a=ice-pwd:%s", c->local_pwd);
		APPEND("a=fingerprint:%s", srv->dtls->fingerprint);
		APPEND("a=setup:passive");
		APPEND("a=mid:%s", c->offer.mid_audio[0] ? c->offer.mid_audio : "1");
		APPEND("a=sendonly");
		/* NOTE: sdes:mid extmap intentionally omitted — pion requires
		 * HandleUndeclaredSSRCWithoutAnswer for PT-based track matching.
		 * go2rtc sets this internally for webrtc: sources. */
		APPEND("a=rtpmap:%d opus/48000/2", c->offer.audio_pt);
		APPEND("a=fmtp:%d minptime=10;useinbandfec=1", c->offer.audio_pt);
		APPEND("a=ssrc:%u cname:raptor", (unsigned)htonl(c->audio_ssrc));
		APPEND("a=candidate:1 1 UDP 2130706431 %s %d typ host", srv->local_ip,
		       srv->udp_port);
	}

#undef APPEND

	return off;
}

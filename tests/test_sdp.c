/*
 * Unit tests for rwd_sdp_parse_offer — the WebRTC SDP offer parser.
 * Highest-value untested attack surface: parses untrusted browser input.
 */

#include "greatest.h"
/* rwd_sdp_offer_t and rwd_sdp_parse_offer provided by fuzz_rwd_shim.h
 * (injected via -include in Makefile to avoid pulling in full rwd.h) */

#include <string.h>

/* ── Minimal valid SDP offer (Chrome-like) ── */

#define VALID_SDP \
	"v=0\r\n" \
	"o=- 1234 1 IN IP4 0.0.0.0\r\n" \
	"s=-\r\n" \
	"t=0 0\r\n" \
	"a=ice-ufrag:abcd\r\n" \
	"a=ice-pwd:efghijklmnopqrstuvwxyz1234\r\n" \
	"a=fingerprint:sha-256 AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99\r\n" \
	"a=setup:actpass\r\n" \
	"m=video 9 UDP/TLS/RTP/SAVPF 96 97\r\n" \
	"a=mid:0\r\n" \
	"a=rtpmap:96 H264/90000\r\n" \
	"a=fmtp:96 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\n" \
	"a=rtpmap:97 VP8/90000\r\n" \
	"m=audio 9 UDP/TLS/RTP/SAVPF 111 0 8\r\n" \
	"a=mid:1\r\n" \
	"a=rtpmap:111 opus/48000/2\r\n" \
	"a=rtpmap:0 PCMU/8000\r\n" \
	"a=rtpmap:8 PCMA/8000\r\n"

TEST sdp_valid_offer(void)
{
	rwd_sdp_offer_t offer;
	int ret = rwd_sdp_parse_offer(VALID_SDP, &offer);
	ASSERT_EQ(0, ret);
	ASSERT_STR_EQ("abcd", offer.ice_ufrag);
	ASSERT_STR_EQ("efghijklmnopqrstuvwxyz1234", offer.ice_pwd);
	ASSERT_STR_EQ("actpass", offer.setup);
	ASSERT(offer.has_video);
	ASSERT(offer.has_audio);
	ASSERT_EQ(96, offer.video_pt);
	ASSERT_EQ(111, offer.audio_pt);
	ASSERT(offer.has_pcmu);
	ASSERT(offer.has_pcma);
	ASSERT_STR_EQ("0", offer.mid_video);
	ASSERT_STR_EQ("1", offer.mid_audio);
	PASS();
}

TEST sdp_video_fmtp(void)
{
	rwd_sdp_offer_t offer;
	rwd_sdp_parse_offer(VALID_SDP, &offer);
	ASSERT(strstr(offer.video_fmtp, "profile-level-id=42e01f"));
	ASSERT(strstr(offer.video_fmtp, "packetization-mode=1"));
	PASS();
}

TEST sdp_fingerprint(void)
{
	rwd_sdp_offer_t offer;
	rwd_sdp_parse_offer(VALID_SDP, &offer);
	ASSERT(strstr(offer.fingerprint, "sha-256"));
	ASSERT(strlen(offer.fingerprint) > 20);
	PASS();
}

TEST sdp_no_ice_ufrag(void)
{
	const char *sdp =
		"v=0\r\n"
		"a=ice-pwd:somepassword1234567890\r\n"
		"a=fingerprint:sha-256 AA:BB\r\n"
		"m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
		"a=rtpmap:96 H264/90000\r\n";
	rwd_sdp_offer_t offer;
	ASSERT(rwd_sdp_parse_offer(sdp, &offer) != 0);
	PASS();
}

TEST sdp_no_fingerprint(void)
{
	const char *sdp =
		"v=0\r\n"
		"a=ice-ufrag:abcd\r\n"
		"a=ice-pwd:somepassword1234567890\r\n"
		"m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
		"a=rtpmap:96 H264/90000\r\n";
	rwd_sdp_offer_t offer;
	ASSERT(rwd_sdp_parse_offer(sdp, &offer) != 0);
	PASS();
}

TEST sdp_no_h264(void)
{
	const char *sdp =
		"v=0\r\n"
		"a=ice-ufrag:abcd\r\n"
		"a=ice-pwd:somepassword1234567890\r\n"
		"a=fingerprint:sha-256 AA:BB\r\n"
		"m=video 9 UDP/TLS/RTP/SAVPF 97\r\n"
		"a=rtpmap:97 VP8/90000\r\n";
	rwd_sdp_offer_t offer;
	ASSERT(rwd_sdp_parse_offer(sdp, &offer) != 0);
	PASS();
}

TEST sdp_video_only(void)
{
	const char *sdp =
		"v=0\r\n"
		"a=ice-ufrag:test\r\n"
		"a=ice-pwd:password1234567890abcdef\r\n"
		"a=fingerprint:sha-256 FF:FF\r\n"
		"a=setup:active\r\n"
		"m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
		"a=rtpmap:96 H264/90000\r\n";
	rwd_sdp_offer_t offer;
	ASSERT_EQ(0, rwd_sdp_parse_offer(sdp, &offer));
	ASSERT(offer.has_video);
	ASSERT_FALSE(offer.has_audio);
	ASSERT_EQ(96, offer.video_pt);
	ASSERT_EQ(-1, offer.audio_pt);
	PASS();
}

TEST sdp_empty_string(void)
{
	rwd_sdp_offer_t offer;
	ASSERT(rwd_sdp_parse_offer("", &offer) != 0);
	PASS();
}

TEST sdp_garbage(void)
{
	rwd_sdp_offer_t offer;
	ASSERT(rwd_sdp_parse_offer("not an sdp at all\r\ngarbage\r\n", &offer) != 0);
	PASS();
}

TEST sdp_no_crlf(void)
{
	/* SDP with plain newlines instead of CRLF — parser uses strstr("\r\n")
	 * so this should fail gracefully (treat as one long line) */
	const char *sdp =
		"v=0\n"
		"a=ice-ufrag:abcd\n"
		"a=ice-pwd:somepassword1234567890\n"
		"a=fingerprint:sha-256 AA:BB\n"
		"m=video 9 UDP/TLS/RTP/SAVPF 96\n"
		"a=rtpmap:96 H264/90000\n";
	rwd_sdp_offer_t offer;
	/* Should fail — parser requires \r\n line endings */
	ASSERT(rwd_sdp_parse_offer(sdp, &offer) != 0);
	PASS();
}

TEST sdp_long_line(void)
{
	/* Line longer than 512-byte internal buffer */
	char sdp[2048];
	int off = 0;
	off += snprintf(sdp + off, sizeof(sdp) - off,
		"v=0\r\n"
		"a=ice-ufrag:abcd\r\n"
		"a=ice-pwd:somepassword1234567890\r\n"
		"a=fingerprint:sha-256 AA:BB\r\n"
		"m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
		"a=rtpmap:96 H264/90000\r\n"
		"a=fmtp:96 ");
	/* Fill with 600 chars of fmtp data */
	for (int i = 0; i < 600 && off < (int)sizeof(sdp) - 3; i++)
		sdp[off++] = 'x';
	sdp[off++] = '\r';
	sdp[off++] = '\n';
	sdp[off] = '\0';

	rwd_sdp_offer_t offer;
	/* Should not crash — line gets truncated to 511 chars */
	int ret = rwd_sdp_parse_offer(sdp, &offer);
	ASSERT_EQ(0, ret);
	ASSERT_EQ(96, offer.video_pt);
	PASS();
}

TEST sdp_candidate_parsing(void)
{
	const char *sdp =
		"v=0\r\n"
		"a=ice-ufrag:cand\r\n"
		"a=ice-pwd:candidatepassword12345678\r\n"
		"a=fingerprint:sha-256 AA:BB\r\n"
		"m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
		"a=rtpmap:96 H264/90000\r\n"
		"a=candidate:1 1 UDP 2122260223 192.168.1.100 54321 typ host\r\n"
		"a=candidate:2 1 UDP 1686052607 203.0.113.5 12345 typ srflx\r\n";
	rwd_sdp_offer_t offer;
	ASSERT_EQ(0, rwd_sdp_parse_offer(sdp, &offer));
	ASSERT_EQ(2, offer.candidate_count);
	ASSERT_STR_EQ("192.168.1.100", offer.candidates[0].ip);
	ASSERT_EQ(54321, offer.candidates[0].port);
	ASSERT_STR_EQ("203.0.113.5", offer.candidates[1].ip);
	ASSERT_EQ(12345, offer.candidates[1].port);
	PASS();
}

TEST sdp_max_candidates(void)
{
	char sdp[4096];
	int off = 0;
	off += snprintf(sdp + off, sizeof(sdp) - off,
		"v=0\r\n"
		"a=ice-ufrag:maxc\r\n"
		"a=ice-pwd:maxcandidatepassword1234\r\n"
		"a=fingerprint:sha-256 AA:BB\r\n"
		"m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
		"a=rtpmap:96 H264/90000\r\n");
	/* 12 candidates — exceeds RWD_MAX_CANDIDATES (8) */
	for (int i = 0; i < 12; i++)
		off += snprintf(sdp + off, sizeof(sdp) - off,
			"a=candidate:%d 1 UDP %u 10.0.0.%d %d typ host\r\n",
			i, 2000000000u - i * 100, i + 1, 50000 + i);

	rwd_sdp_offer_t offer;
	ASSERT_EQ(0, rwd_sdp_parse_offer(sdp, &offer));
	ASSERT_EQ(RWD_MAX_CANDIDATES, offer.candidate_count);
	PASS();
}

TEST sdp_extmap_sdes_mid(void)
{
	const char *sdp =
		"v=0\r\n"
		"a=ice-ufrag:ext1\r\n"
		"a=ice-pwd:extmappassword1234567890\r\n"
		"a=fingerprint:sha-256 AA:BB\r\n"
		"m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
		"a=rtpmap:96 H264/90000\r\n"
		"a=extmap:4 urn:ietf:params:rtp-hdrext:sdes:mid\r\n";
	rwd_sdp_offer_t offer;
	ASSERT_EQ(0, rwd_sdp_parse_offer(sdp, &offer));
	ASSERT_EQ(4, offer.mid_ext_id);
	PASS();
}

TEST sdp_multiple_video_codecs(void)
{
	/* Browser offers VP8 before H264 — parser should find H264 */
	const char *sdp =
		"v=0\r\n"
		"a=ice-ufrag:multi\r\n"
		"a=ice-pwd:multicodecpassword123456\r\n"
		"a=fingerprint:sha-256 AA:BB\r\n"
		"m=video 9 UDP/TLS/RTP/SAVPF 97 96 98\r\n"
		"a=rtpmap:97 VP8/90000\r\n"
		"a=rtpmap:96 H264/90000\r\n"
		"a=rtpmap:98 VP9/90000\r\n";
	rwd_sdp_offer_t offer;
	ASSERT_EQ(0, rwd_sdp_parse_offer(sdp, &offer));
	ASSERT_EQ(96, offer.video_pt);
	PASS();
}

TEST sdp_case_insensitive_codec(void)
{
	const char *sdp =
		"v=0\r\n"
		"a=ice-ufrag:case\r\n"
		"a=ice-pwd:caseinsensitivepassword1\r\n"
		"a=fingerprint:sha-256 AA:BB\r\n"
		"m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
		"a=rtpmap:96 h264/90000\r\n"
		"m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
		"a=rtpmap:111 OPUS/48000/2\r\n";
	rwd_sdp_offer_t offer;
	ASSERT_EQ(0, rwd_sdp_parse_offer(sdp, &offer));
	ASSERT_EQ(96, offer.video_pt);
	ASSERT_EQ(111, offer.audio_pt);
	PASS();
}

TEST sdp_invalid_pt_range(void)
{
	const char *sdp =
		"v=0\r\n"
		"a=ice-ufrag:ptbad\r\n"
		"a=ice-pwd:ptrangepassword123456789\r\n"
		"a=fingerprint:sha-256 AA:BB\r\n"
		"m=video 9 UDP/TLS/RTP/SAVPF 200\r\n"
		"a=rtpmap:200 H264/90000\r\n";
	rwd_sdp_offer_t offer;
	/* PT 200 > 127, parser should reject it */
	ASSERT(rwd_sdp_parse_offer(sdp, &offer) != 0);
	PASS();
}

SUITE(sdp_suite)
{
	RUN_TEST(sdp_valid_offer);
	RUN_TEST(sdp_video_fmtp);
	RUN_TEST(sdp_fingerprint);
	RUN_TEST(sdp_no_ice_ufrag);
	RUN_TEST(sdp_no_fingerprint);
	RUN_TEST(sdp_no_h264);
	RUN_TEST(sdp_video_only);
	RUN_TEST(sdp_empty_string);
	RUN_TEST(sdp_garbage);
	RUN_TEST(sdp_no_crlf);
	RUN_TEST(sdp_long_line);
	RUN_TEST(sdp_candidate_parsing);
	RUN_TEST(sdp_max_candidates);
	RUN_TEST(sdp_extmap_sdes_mid);
	RUN_TEST(sdp_multiple_video_codecs);
	RUN_TEST(sdp_case_insensitive_codec);
	RUN_TEST(sdp_invalid_pt_range);
}

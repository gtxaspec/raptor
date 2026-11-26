/*
 * test_wt_proto.c -- WebTorrent tracker wire forms
 */

#include <stdint.h>
#include <string.h>

#include "greatest.h"
#include "../rwd/rwd_wt_proto.h"

TEST binary_string_round_trip(void)
{
	uint8_t in[WT_ID_LEN], back[WT_ID_LEN];
	for (int i = 0; i < WT_ID_LEN; i++)
		in[i] = (uint8_t)(i * 13 + 0x7a); /* crosses 0x80 and wraps through 0x00 */
	char js[2 * WT_ID_LEN + 1];
	wt_bin_to_json(in, WT_ID_LEN, js);
	ASSERT(wt_json_to_bin(js, back, WT_ID_LEN));
	ASSERT_MEM_EQ(in, back, WT_ID_LEN);
	PASS();
}

TEST binary_string_matches_javascript(void)
{
	/* String.fromCharCode(0x41, 0xe9, 0xff, 0x80) as UTF-8 */
	const uint8_t in[4] = {0x41, 0xe9, 0xff, 0x80};
	char js[9];
	wt_bin_to_json(in, 4, js);
	ASSERT_STR_EQ("A\xc3\xa9\xc3\xbf\xc2\x80", js);
	PASS();
}

TEST binary_string_rejects_other_lengths_and_wide_chars(void)
{
	uint8_t out[WT_ID_LEN];
	ASSERT_FALSE(wt_json_to_bin("s1_abcdef1234", out, WT_ID_LEN));		    /* 13 */
	ASSERT_FALSE(wt_json_to_bin("0123456789abcdef0123456789", out, WT_ID_LEN)); /* 26 */
	/* 19 ASCII + U+0100 (two bytes, above the binary range) */
	ASSERT_FALSE(wt_json_to_bin("0123456789abcdef012\xc4\x80", out, WT_ID_LEN));
	/* a stray continuation byte */
	ASSERT_FALSE(wt_json_to_bin("0123456789abcdef012\x80", out, WT_ID_LEN));
	ASSERT(wt_json_to_bin("-RP0001-abcdef012345", out, WT_ID_LEN));
	PASS();
}

TEST sdp_attr_taken_and_line_removed(void)
{
	char sdp[] = "v=0\r\na=x-raptor-stream:1\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
		     "a=x-raptor-auth:abcd\r\n";
	char v[80];
	ASSERT(wt_sdp_take_attr(sdp, "x-raptor-auth", v, sizeof(v)));
	ASSERT_STR_EQ("abcd", v);
	ASSERT(wt_sdp_take_attr(sdp, "x-raptor-stream", v, sizeof(v)));
	ASSERT_STR_EQ("1", v);
	ASSERT_STR_EQ("v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\n", sdp);
	PASS();
}

TEST sdp_attr_missing_prefix_or_too_long(void)
{
	char sdp[] = "v=0\r\na=x-raptor-authx:1\r\na=x-raptor-auth:0123456789\n";
	char before[sizeof(sdp)];
	memcpy(before, sdp, sizeof(sdp));
	char v[8];
	ASSERT_FALSE(wt_sdp_take_attr(sdp, "x-raptor-stream", v, sizeof(v)));
	ASSERT_FALSE(wt_sdp_take_attr(sdp, "x-raptor-auth", v, sizeof(v))); /* value too long */
	ASSERT_STR_EQ(before, sdp);
	char big[16];
	ASSERT(wt_sdp_take_attr(sdp, "x-raptor-auth", big, sizeof(big))); /* LF-only line */
	ASSERT_STR_EQ("0123456789", big);
	ASSERT_STR_EQ("v=0\r\na=x-raptor-authx:1\r\n", sdp);
	PASS();
}

SUITE(wt_proto_suite)
{
	RUN_TEST(binary_string_round_trip);
	RUN_TEST(binary_string_matches_javascript);
	RUN_TEST(binary_string_rejects_other_lengths_and_wide_chars);
	RUN_TEST(sdp_attr_taken_and_line_removed);
	RUN_TEST(sdp_attr_missing_prefix_or_too_long);
}

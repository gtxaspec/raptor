/*
 * test_rtp_info.c -- the RTP-Info value never outgrows its buffer
 */

#include <stdlib.h>
#include <string.h>

#include "greatest.h"
#include "../rsd/rsd_rtp_info.h"

static const rsd_rtp_info_track_t V = {true, 7, 9}, A = {true, 11, 13}, OFF = {false, 0, 0};

#define URI "rtsp://cam/stream0"

/* An exact heap allocation, so a write past cap is an ASan report */
static int build(size_t cap, const char *uri, const rsd_rtp_info_track_t *v,
		 const rsd_rtp_info_track_t *a, char *copy, size_t copy_len)
{
	char *out = malloc(cap);
	if (!out)
		abort();
	int n = rsd_rtp_info_build(out, cap, uri, strlen(uri), v, a);
	snprintf(copy, copy_len, "%s", n > 0 ? out : "");
	free(out);
	return n;
}

TEST both_tracks(void)
{
	char s[256];
	const char *want = "url=" URI "/video;seq=7;rtptime=9,url=" URI "/audio;seq=11;rtptime=13";
	ASSERT_EQ((int)strlen(want), build(512, URI, &V, &A, s, sizeof(s)));
	ASSERT_STR_EQ(want, s);
	PASS();
}

TEST one_track_each(void)
{
	char s[256];
	build(512, URI, &V, &OFF, s, sizeof(s));
	ASSERT_STR_EQ("url=" URI "/video;seq=7;rtptime=9", s);
	build(512, URI, &OFF, &A, s, sizeof(s));
	ASSERT_STR_EQ("url=" URI "/audio;seq=11;rtptime=13", s);
	PASS();
}

TEST no_track_is_empty(void)
{
	char s[16];
	ASSERT_EQ(0, build(512, URI, &OFF, &OFF, s, sizeof(s)));
	PASS();
}

TEST trailing_slashes_are_dropped(void)
{
	char s[256];
	build(512, URI "///", &V, &OFF, s, sizeof(s));
	ASSERT_STR_EQ("url=" URI "/video;seq=7;rtptime=9", s);
	PASS();
}

/* The field bug: a URI the buffer cannot carry twice */
TEST a_long_uri_is_refused_not_written(void)
{
	char uri[900], s[16];
	memset(uri, 'A', sizeof(uri) - 1);
	uri[sizeof(uri) - 1] = '\0';
	memcpy(uri, "rtsp://cam/", 11);
	for (size_t len = 200; len < sizeof(uri); len += 50) {
		char cut[900];
		snprintf(cut, sizeof(cut), "%.*s", (int)len, uri);
		int n = build(512, cut, &V, &A, s, sizeof(s));
		ASSERT(n == -1 || (n > 0 && n < 512));
	}
	ASSERT_EQ(-1, build(512, uri, &V, &A, s, sizeof(s)));
	ASSERT_EQ(-1, build(512, uri, &V, &OFF, s, sizeof(s)));
	PASS();
}

TEST video_fitting_alone_is_not_enough(void)
{
	char uri[300], s[16];
	memset(uri, 'B', sizeof(uri) - 1);
	uri[sizeof(uri) - 1] = '\0';
	ASSERT(build(512, uri, &V, &OFF, s, sizeof(s)) > 0);
	ASSERT_EQ(-1, build(512, uri, &V, &A, s, sizeof(s)));
	PASS();
}

TEST the_last_byte_decides(void)
{
	char s[256];
	const char *want = "url=" URI "/video;seq=7;rtptime=9";
	size_t len = strlen(want);
	ASSERT_EQ((int)len, build(len + 1, URI, &V, &OFF, s, sizeof(s)));
	ASSERT_STR_EQ(want, s);
	ASSERT_EQ(-1, build(len, URI, &V, &OFF, s, sizeof(s)));
	PASS();
}

/* RFC 2326 12.33 / the dev guide: ffmpeg strips credentials from the URL it
 * requested and then fails to match an RTP-Info url that still carries them.
 * The strip in the first implementation stopped at the "/" of "rtsp://" and
 * never matched. */
static void url_of(const char *uri, char *s, size_t n)
{
	build(512, uri, &V, &OFF, s, n);
}

TEST userinfo_is_stripped(void)
{
	char s[256];
	url_of("rtsp://user:pass@cam/stream0", s, sizeof(s));
	ASSERT_STR_EQ("url=rtsp://cam/stream0/video;seq=7;rtptime=9", s);
	url_of("rtsp://user@cam:554/stream0", s, sizeof(s));
	ASSERT_STR_EQ("url=rtsp://cam:554/stream0/video;seq=7;rtptime=9", s);
	url_of("rtsps://@cam/stream0", s, sizeof(s));
	ASSERT_STR_EQ("url=rtsps://cam/stream0/video;seq=7;rtptime=9", s);
	PASS();
}

TEST an_unescaped_at_in_the_password_does_not_leak_its_tail(void)
{
	char s[256];
	url_of("rtsp://u:p@ss@cam/stream0", s, sizeof(s));
	ASSERT_STR_EQ("url=rtsp://cam/stream0/video;seq=7;rtptime=9", s);
	PASS();
}

TEST an_ipv6_host_keeps_its_brackets(void)
{
	char s[256];
	url_of("rtsp://u:p@[fe80::1]:554/stream0", s, sizeof(s));
	ASSERT_STR_EQ("url=rtsp://[fe80::1]:554/stream0/video;seq=7;rtptime=9", s);
	PASS();
}

TEST an_at_after_the_authority_is_path_not_credentials(void)
{
	char s[256];
	url_of("rtsp://cam/a@b", s, sizeof(s));
	ASSERT_STR_EQ("url=rtsp://cam/a@b/video;seq=7;rtptime=9", s);
	url_of("rtsp://cam/stream0?t=a@b", s, sizeof(s));
	ASSERT_STR_EQ("url=rtsp://cam/stream0?t=a@b/video;seq=7;rtptime=9", s);
	url_of("rtsp://cam", s, sizeof(s));
	ASSERT_STR_EQ("url=rtsp://cam/video;seq=7;rtptime=9", s);
	PASS();
}

TEST a_uri_without_a_scheme_is_left_alone(void)
{
	char s[256];
	url_of("/stream0/a@b", s, sizeof(s));
	ASSERT_STR_EQ("url=/stream0/a@b/video;seq=7;rtptime=9", s);
	PASS();
}

TEST stripping_both_tracks_and_slashes_together(void)
{
	char s[256];
	build(512, "rtsp://u:p@cam/stream0//", &V, &A, s, sizeof(s));
	ASSERT_STR_EQ("url=rtsp://cam/stream0/video;seq=7;rtptime=9,"
		      "url=rtsp://cam/stream0/audio;seq=11;rtptime=13",
		      s);
	PASS();
}

SUITE(rtp_info_suite)
{
	RUN_TEST(both_tracks);
	RUN_TEST(one_track_each);
	RUN_TEST(no_track_is_empty);
	RUN_TEST(trailing_slashes_are_dropped);
	RUN_TEST(a_long_uri_is_refused_not_written);
	RUN_TEST(video_fitting_alone_is_not_enough);
	RUN_TEST(the_last_byte_decides);
	RUN_TEST(userinfo_is_stripped);
	RUN_TEST(an_unescaped_at_in_the_password_does_not_leak_its_tail);
	RUN_TEST(an_ipv6_host_keeps_its_brackets);
	RUN_TEST(an_at_after_the_authority_is_path_not_credentials);
	RUN_TEST(a_uri_without_a_scheme_is_left_alone);
	RUN_TEST(stripping_both_tracks_and_slashes_together);
}

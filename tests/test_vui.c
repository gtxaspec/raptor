/*
 * rss_vui_set_full_range: correct the coded range declaration in an
 * SPS VUI without changing the escaped byte length.
 *
 * The two positive fixtures are real SPS NALs captured from the T31
 * encoder (H.264 High at 2560x1440 and the HEVC main-stream twin);
 * both carry the video_signal_type block declaring limited range, and
 * both contain emulation-prevention sequences the edit must preserve.
 */

#include <string.h>

#include "greatest.h"
#include "rss_vui.h"

/* 2560x1440 H.264 High SPS, video_full_range_flag = 0. */
static const uint8_t sps264[] = {0x27, 0x64, 0x00, 0x33, 0xad, 0x00, 0xce, 0x80, 0x28, 0x00,
				 0xb5, 0xa6, 0xa0, 0x20, 0x20, 0x3e, 0x00, 0x00, 0x03, 0x00,
				 0x02, 0x00, 0x00, 0x03, 0x00, 0x78, 0x60, 0x40, 0x00, 0x2d,
				 0xc6, 0xc0, 0x00, 0x11, 0x2a, 0x8f, 0xff, 0xf8, 0x14};

/* 2560x1440 H.265 SPS, video_full_range_flag = 0. */
static const uint8_t sps265[] = {
	0x42, 0x01, 0x01, 0x21, 0x40, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00,
	0x03, 0x00, 0x96, 0xa0, 0x01, 0x40, 0x20, 0x05, 0xa1, 0x67, 0xae, 0xe4, 0x4a, 0x17, 0x35,
	0x01, 0x01, 0x01, 0x04, 0x00, 0x00, 0x03, 0x00, 0x04, 0x00, 0x00, 0x03, 0x00, 0x78, 0x20};

TEST vui_h264_flip_and_idempotent(void)
{
	uint8_t nal[sizeof(sps264)];
	memcpy(nal, sps264, sizeof(nal));

	ASSERT_EQ(1, rss_vui_set_full_range(nal, sizeof(nal), 0));
	/* One byte differs, everything else is untouched. */
	int diffs = 0;
	for (size_t i = 0; i < sizeof(nal); i++)
		if (nal[i] != sps264[i])
			diffs++;
	ASSERT_EQ(1, diffs);
	/* A second pass finds the flag already set. */
	ASSERT_EQ(0, rss_vui_set_full_range(nal, sizeof(nal), 0));
	PASS();
}

TEST vui_h265_flip_and_idempotent(void)
{
	uint8_t nal[sizeof(sps265)];
	memcpy(nal, sps265, sizeof(nal));

	ASSERT_EQ(1, rss_vui_set_full_range(nal, sizeof(nal), 1));
	int diffs = 0;
	for (size_t i = 0; i < sizeof(nal); i++)
		if (nal[i] != sps265[i])
			diffs++;
	ASSERT_EQ(1, diffs);
	ASSERT_EQ(0, rss_vui_set_full_range(nal, sizeof(nal), 1));
	PASS();
}

TEST vui_h264_matrix_and_idempotent(void)
{
	uint8_t nal[sizeof(sps264)];
	memcpy(nal, sps264, sizeof(nal));

	/* The capture declares bt709 (1): rewriting to the same value is
	 * a no-op that touches nothing. */
	ASSERT_EQ(0, rss_vui_set_matrix(nal, sizeof(nal), 0, 1));
	ASSERT_EQ(0, memcmp(nal, sps264, sizeof(nal)));
	/* Rewriting to smpte170m (6) edits once, then reports done. */
	ASSERT_EQ(1, rss_vui_set_matrix(nal, sizeof(nal), 0, 6));
	ASSERT_EQ(0, rss_vui_set_matrix(nal, sizeof(nal), 0, 6));
	/* The matrix edit leaves the range flag intact and editable. */
	ASSERT_EQ(1, rss_vui_set_full_range(nal, sizeof(nal), 0));
	ASSERT_EQ(1, rss_vui_set_matrix(nal, sizeof(nal), 0, 1));
	PASS();
}

TEST vui_h265_matrix_and_idempotent(void)
{
	uint8_t nal[sizeof(sps265)];
	memcpy(nal, sps265, sizeof(nal));

	ASSERT_EQ(0, rss_vui_set_matrix(nal, sizeof(nal), 1, 1));
	ASSERT_EQ(0, memcmp(nal, sps265, sizeof(nal)));
	ASSERT_EQ(1, rss_vui_set_matrix(nal, sizeof(nal), 1, 6));
	ASSERT_EQ(0, rss_vui_set_matrix(nal, sizeof(nal), 1, 6));
	ASSERT_EQ(1, rss_vui_set_full_range(nal, sizeof(nal), 1));
	ASSERT_EQ(1, rss_vui_set_matrix(nal, sizeof(nal), 1, 5));
	PASS();
}

TEST vui_matrix_truncated_never_grows_or_loops(void)
{
	/* Same contract as the range sweep: any outcome is fine on
	 * garbage input as long as writes stay inside the given length
	 * and a successful edit is idempotent. */
	for (size_t cut = 5; cut < sizeof(sps264); cut++) {
		uint8_t nal[sizeof(sps264)];
		memcpy(nal, sps264, sizeof(nal));
		int rc = rss_vui_set_matrix(nal, (uint32_t)cut, 0, 6);
		if (rc == 1)
			ASSERT_EQ(0, rss_vui_set_matrix(nal, (uint32_t)cut, 0, 6));
		ASSERT_EQ(0, memcmp(nal + cut, sps264 + cut, sizeof(nal) - cut));
	}
	PASS();
}

TEST vui_accepts_annexb_start_code(void)
{
	uint8_t nal[4 + sizeof(sps264)] = {0x00, 0x00, 0x00, 0x01};
	memcpy(nal + 4, sps264, sizeof(sps264));
	ASSERT_EQ(1, rss_vui_set_full_range(nal, sizeof(nal), 0));
	ASSERT_EQ(0, rss_vui_set_full_range(nal, sizeof(nal), 0));
	PASS();
}

TEST vui_no_vui_is_a_noop(void)
{
	/* Minimal baseline SPS, vui_parameters_present_flag = 0:
	 * 1920x1080-ish fields hand-assembled, no VUI at the end. */
	uint8_t nal[] = {0x67, 0x42, 0x00, 0x1e, 0x8c, 0x8d, 0x40, 0x3c,
			 0x22, 0x11, 0xa8, 0x00, 0x00, 0x1f, 0x08};
	uint8_t before[sizeof(nal)];
	memcpy(before, nal, sizeof(nal));
	int rc = rss_vui_set_full_range(nal, sizeof(nal), 0);
	ASSERT(rc == 0 || rc == -1);
	ASSERT_EQ(0, memcmp(before, nal, sizeof(nal)));
	PASS();
}

TEST vui_rejects_wrong_nal_type(void)
{
	uint8_t pps[] = {0x28, 0xee, 0x3c, 0xb0};
	ASSERT_EQ(-1, rss_vui_set_full_range(pps, sizeof(pps), 0));
	uint8_t slice265[] = {0x02, 0x01, 0xd0, 0x09, 0x7e, 0x10};
	ASSERT_EQ(-1, rss_vui_set_full_range(slice265, sizeof(slice265), 1));
	PASS();
}

TEST vui_truncated_never_grows_or_loops(void)
{
	/* A truncated SPS is garbage-in: the walk may land anywhere, and
	 * accidentally parsing is allowed. What must hold on any input is
	 * the contract: in-bounds access only (the sanitizer build checks
	 * that), the byte length never changes, at most one byte is
	 * modified, and a second pass is a no-op. */
	for (size_t cut = 5; cut < sizeof(sps264); cut++) {
		uint8_t nal[sizeof(sps264)];
		memcpy(nal, sps264, sizeof(nal));
		uint8_t before[sizeof(nal)];
		memcpy(before, nal, sizeof(nal));
		int rc = rss_vui_set_full_range(nal, (uint32_t)cut, 0);
		int diffs = 0;
		for (size_t i = 0; i < sizeof(nal); i++)
			if (nal[i] != before[i])
				diffs++;
		if (rc == 1) {
			ASSERT_EQ(1, diffs);
			ASSERT_EQ(0, rss_vui_set_full_range(nal, (uint32_t)cut, 0));
		} else {
			ASSERT_EQ(0, diffs);
		}
	}
	PASS();
}

SUITE(vui_suite)
{
	RUN_TEST(vui_h264_flip_and_idempotent);
	RUN_TEST(vui_h265_flip_and_idempotent);
	RUN_TEST(vui_h264_matrix_and_idempotent);
	RUN_TEST(vui_h265_matrix_and_idempotent);
	RUN_TEST(vui_matrix_truncated_never_grows_or_loops);
	RUN_TEST(vui_accepts_annexb_start_code);
	RUN_TEST(vui_no_vui_is_a_noop);
	RUN_TEST(vui_rejects_wrong_nal_type);
	RUN_TEST(vui_truncated_never_grows_or_loops);
}

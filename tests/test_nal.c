#include "greatest.h"
#include "rmr_nal.h"

#include <string.h>

/*
 * H.264 NAL types: SPS=7 (0x67), PPS=8 (0x68), IDR=5 (0x65), non-IDR=1 (0x41)
 * H.265 NAL types: VPS=32 (0x40), SPS=33 (0x42), PPS=34 (0x44), IDR=19 (0x26)
 * Annex B: 00 00 00 01 [NAL] or 00 00 01 [NAL]
 */

TEST annexb_to_avcc_h264(void)
{
	/* SPS + PPS + IDR + P-frame (4-byte start codes) */
	uint8_t src[] = {
		0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xC0, 0x1E, /* SPS */
		0x00, 0x00, 0x00, 0x01, 0x68, 0xCE, 0x38, 0x80, /* PPS */
		0x00, 0x00, 0x00, 0x01, 0x65, 0xAA, 0xBB,       /* IDR */
		0x00, 0x00, 0x00, 0x01, 0x41, 0xCC,              /* non-IDR */
	};
	uint8_t dst[256];
	int len = rmr_annexb_to_avcc(src, sizeof(src), dst, sizeof(dst), 0);
	ASSERT(len > 0);

	/* SPS and PPS should be skipped; only IDR + non-IDR remain */
	/* IDR: 3 bytes (0x65 0xAA 0xBB) → 4-byte len + 3 bytes = 7 */
	/* non-IDR: 2 bytes (0x41 0xCC) → 4-byte len + 2 bytes = 6 */
	ASSERT_EQ(7 + 6, len);

	/* Check IDR length prefix (big-endian 3) */
	ASSERT_EQ(0, dst[0]);
	ASSERT_EQ(0, dst[1]);
	ASSERT_EQ(0, dst[2]);
	ASSERT_EQ(3, dst[3]);
	ASSERT_EQ(0x65, dst[4]); /* IDR NAL type */

	/* Check non-IDR length prefix (big-endian 2) */
	ASSERT_EQ(0, dst[7]);
	ASSERT_EQ(0, dst[8]);
	ASSERT_EQ(0, dst[9]);
	ASSERT_EQ(2, dst[10]);
	ASSERT_EQ(0x41, dst[11]); /* non-IDR NAL type */
	PASS();
}

TEST annexb_to_avcc_3byte(void)
{
	/* 3-byte start codes */
	uint8_t src[] = {
		0x00, 0x00, 0x01, 0x65, 0xAA, /* IDR (3-byte sc) */
		0x00, 0x00, 0x01, 0x41, 0xBB, /* non-IDR (3-byte sc) */
	};
	uint8_t dst[64];
	int len = rmr_annexb_to_avcc(src, sizeof(src), dst, sizeof(dst), 0);
	ASSERT(len > 0);
	/* IDR: 2 bytes + non-IDR: 2 bytes → (4+2) + (4+2) = 12 */
	ASSERT_EQ(12, len);
	ASSERT_EQ(0x65, dst[4]);
	ASSERT_EQ(0x41, dst[10]);
	PASS();
}

TEST annexb_to_avcc_h265(void)
{
	/* H.265: VPS(type 32=0x40) + SPS(type 33=0x42) + PPS(type 34=0x44) + IDR(type 19=0x26) */
	uint8_t src[] = {
		0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0xAA, /* VPS (nal_type = (0x40>>1)&0x3F = 32) */
		0x00, 0x00, 0x00, 0x01, 0x42, 0x01, 0xBB, /* SPS (nal_type = (0x42>>1)&0x3F = 33) */
		0x00, 0x00, 0x00, 0x01, 0x44, 0x01, 0xCC, /* PPS (nal_type = (0x44>>1)&0x3F = 34) */
		0x00, 0x00, 0x00, 0x01, 0x26, 0x01, 0xDD, /* IDR (nal_type = (0x26>>1)&0x3F = 19) */
	};
	uint8_t dst[64];
	int len = rmr_annexb_to_avcc(src, sizeof(src), dst, sizeof(dst), 1);
	ASSERT(len > 0);
	/* Only IDR NAL should pass through: 3 bytes → 4 + 3 = 7 */
	ASSERT_EQ(7, len);
	ASSERT_EQ(0x26, dst[4]);
	PASS();
}

TEST annexb_to_avcc_empty(void)
{
	uint8_t dst[16];
	int len = rmr_annexb_to_avcc(NULL, 0, dst, sizeof(dst), 0);
	ASSERT_EQ(0, len);
	PASS();
}

TEST annexb_to_avcc_overflow(void)
{
	uint8_t src[] = {
		0x00, 0x00, 0x00, 0x01, 0x65, 0xAA, 0xBB, 0xCC, 0xDD,
	};
	uint8_t dst[4]; /* too small for 4-byte prefix + 5-byte NAL */
	int len = rmr_annexb_to_avcc(src, sizeof(src), dst, sizeof(dst), 0);
	ASSERT_EQ(-1, len);
	PASS();
}

TEST annexb_to_avcc_single_nal(void)
{
	uint8_t src[] = {
		0x00, 0x00, 0x00, 0x01, 0x41, 0xFF,
	};
	uint8_t dst[32];
	int len = rmr_annexb_to_avcc(src, sizeof(src), dst, sizeof(dst), 0);
	ASSERT_EQ(6, len); /* 4-byte prefix + 2-byte NAL */
	ASSERT_EQ(0x41, dst[4]);
	ASSERT_EQ(0xFF, dst[5]);
	PASS();
}

TEST extract_params_h264(void)
{
	uint8_t keyframe[] = {
		0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xC0, 0x1E, /* SPS */
		0x00, 0x00, 0x00, 0x01, 0x68, 0xCE, 0x38, 0x80, /* PPS */
		0x00, 0x00, 0x00, 0x01, 0x65, 0xAA, 0xBB,       /* IDR */
	};
	rmr_codec_params_t params = {0};
	rmr_extract_params(keyframe, sizeof(keyframe), 0, &params);
	ASSERT(params.ready);
	ASSERT_EQ(4u, params.sps_len); /* 0x67 0x42 0xC0 0x1E */
	ASSERT_EQ(0x67, params.sps[0]);
	ASSERT_EQ(4u, params.pps_len); /* 0x68 0xCE 0x38 0x80 */
	ASSERT_EQ(0x68, params.pps[0]);
	PASS();
}

TEST extract_params_h265(void)
{
	uint8_t keyframe[] = {
		0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0xAA, /* VPS */
		0x00, 0x00, 0x00, 0x01, 0x42, 0x01, 0xBB, /* SPS */
		0x00, 0x00, 0x00, 0x01, 0x44, 0x01, 0xCC, /* PPS */
		0x00, 0x00, 0x00, 0x01, 0x26, 0x01, 0xDD, /* IDR */
	};
	rmr_codec_params_t params = {0};
	rmr_extract_params(keyframe, sizeof(keyframe), 1, &params);
	ASSERT(params.ready);
	ASSERT(params.vps_len > 0);
	ASSERT_EQ(0x40, params.vps[0]);
	ASSERT(params.sps_len > 0);
	ASSERT_EQ(0x42, params.sps[0]);
	ASSERT(params.pps_len > 0);
	ASSERT_EQ(0x44, params.pps[0]);
	PASS();
}

TEST extract_params_no_params(void)
{
	/* Non-keyframe: only P-frame NALs, no SPS/PPS */
	uint8_t frame[] = {
		0x00, 0x00, 0x00, 0x01, 0x41, 0xCC, 0xDD,
	};
	rmr_codec_params_t params = {0};
	rmr_extract_params(frame, sizeof(frame), 0, &params);
	ASSERT_FALSE(params.ready);
	ASSERT_EQ(0u, params.sps_len);
	ASSERT_EQ(0u, params.pps_len);
	PASS();
}

TEST annexb_to_avcc_mixed_sc(void)
{
	/* Mix of 3-byte and 4-byte start codes in one frame */
	uint8_t src[] = {
		0x00, 0x00, 0x00, 0x01, 0x65, 0xAA,       /* IDR (4-byte sc) */
		0x00, 0x00, 0x01, 0x41, 0xBB, 0xCC,        /* non-IDR (3-byte sc) */
		0x00, 0x00, 0x00, 0x01, 0x41, 0xDD,        /* non-IDR (4-byte sc) */
	};
	uint8_t dst[128];
	int len = rmr_annexb_to_avcc(src, sizeof(src), dst, sizeof(dst), 0);
	ASSERT(len > 0);
	/* IDR(2) + non-IDR(3, includes 0xBB 0xCC) + non-IDR(2) → 6+7+6 = 19 */
	ASSERT_EQ(19, len);
	ASSERT_EQ(0x65, dst[4]);  /* first NAL */
	ASSERT_EQ(0x41, dst[10]); /* second NAL */
	ASSERT_EQ(0x41, dst[17]); /* third NAL */
	PASS();
}

TEST annexb_to_avcc_trailing_zeros(void)
{
	/* NAL data followed by trailing zeros (padding between start codes) */
	uint8_t src[] = {
		0x00, 0x00, 0x00, 0x01, 0x41, 0xAA, 0xBB, /* non-IDR */
		0x00, 0x00, 0x00,                           /* trailing zeros before next sc */
		0x00, 0x00, 0x00, 0x01, 0x41, 0xCC,         /* next non-IDR */
	};
	uint8_t dst[64];
	int len = rmr_annexb_to_avcc(src, sizeof(src), dst, sizeof(dst), 0);
	ASSERT(len > 0);
	/* First NAL: 0x41 0xAA 0xBB = 3 bytes (trailing zeros stripped) */
	ASSERT_EQ(3, (int)((uint32_t)dst[3]));
	PASS();
}

TEST annexb_to_avcc_large_nal(void)
{
	/* NAL > 255 bytes to test 4-byte length encoding */
	uint8_t src[520];
	src[0] = 0x00; src[1] = 0x00; src[2] = 0x00; src[3] = 0x01;
	src[4] = 0x65; /* IDR */
	memset(src + 5, 0xAB, 515);
	uint8_t dst[1024];
	int len = rmr_annexb_to_avcc(src, sizeof(src), dst, sizeof(dst), 0);
	ASSERT(len > 0);
	/* NAL is 516 bytes (0x65 + 515 pad) */
	uint32_t nal_len = ((uint32_t)dst[0] << 24) | ((uint32_t)dst[1] << 16) |
			   ((uint32_t)dst[2] << 8) | dst[3];
	ASSERT_EQ(516, (int)nal_len);
	ASSERT_EQ(520, len); /* 4 + 516 */
	PASS();
}

SUITE(nal_suite)
{
	RUN_TEST(annexb_to_avcc_h264);
	RUN_TEST(annexb_to_avcc_3byte);
	RUN_TEST(annexb_to_avcc_h265);
	RUN_TEST(annexb_to_avcc_empty);
	RUN_TEST(annexb_to_avcc_overflow);
	RUN_TEST(annexb_to_avcc_single_nal);
	RUN_TEST(annexb_to_avcc_mixed_sc);
	RUN_TEST(annexb_to_avcc_trailing_zeros);
	RUN_TEST(annexb_to_avcc_large_nal);
	RUN_TEST(extract_params_h264);
	RUN_TEST(extract_params_h265);
	RUN_TEST(extract_params_no_params);
}

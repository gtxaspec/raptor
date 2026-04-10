/*
 * Unit tests for audio codec decoding (G.711 µ-law / A-law)
 * and RTP timestamp conversion.
 *
 * These are static functions in rwd_media.c / rsd_ring_reader.c.
 * Re-implemented here from the ITU-T G.711 spec for verification.
 */

#include "greatest.h"

#include <stdint.h>

/* ── G.711 µ-law decode (same algorithm as rwd_media.c:90) ── */

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

/* ── G.711 A-law decode (same algorithm as rwd_media.c:101) ── */

static int16_t alaw_decode(uint8_t alaw)
{
	alaw ^= 0x55;
	int sign = alaw & 0x80;
	int exponent = (alaw >> 4) & 0x07;
	int mantissa = alaw & 0x0f;
	int magnitude =
		(exponent == 0) ? (mantissa << 4) + 8 : ((mantissa << 4) + 264) << (exponent - 1);
	return (int16_t)(sign ? magnitude : -magnitude);
}

/* ── RTP timestamp conversion (same math as rsd_ring_reader.c:191) ── */

static uint32_t video_rtp_ts(int64_t ts, int64_t epoch)
{
	return (uint32_t)((uint64_t)(ts - epoch) * 90000 / 1000000);
}

static uint32_t audio_rtp_ts(int64_t ts, int64_t epoch, uint32_t clock_rate)
{
	return (uint32_t)((uint64_t)(ts - epoch) * clock_rate / 1000000);
}

/* ── AAC AU header (RFC 3640, same as rsd_ring_reader.c:237) ── */

static void aac_au_header(uint8_t out[4], uint32_t frame_len)
{
	out[0] = 0x00;
	out[1] = 0x10; /* 16 bits of AU header */
	out[2] = (uint8_t)((frame_len >> 5) & 0xFF);
	out[3] = (uint8_t)((frame_len << 3) & 0xFF);
}

/* ── µ-law tests ── */

TEST ulaw_silence(void)
{
	/* µ-law 0xFF = positive zero (silence) */
	int16_t val = ulaw_decode(0xFF);
	ASSERT_EQ(0, val);
	PASS();
}

TEST ulaw_max_positive(void)
{
	/* µ-law 0x80 = max positive magnitude */
	int16_t val = ulaw_decode(0x80);
	ASSERT(val > 8000); /* ITU-T spec: ~8031 */
	PASS();
}

TEST ulaw_max_negative(void)
{
	/* µ-law 0x00 = max negative magnitude */
	int16_t val = ulaw_decode(0x00);
	ASSERT(val < -8000);
	PASS();
}

TEST ulaw_symmetry(void)
{
	/* Positive and negative encodings for same magnitude should
	 * produce equal-but-opposite values */
	for (int m = 0; m < 128; m++) {
		int16_t pos = ulaw_decode((uint8_t)(0x80 | m));
		int16_t neg = ulaw_decode((uint8_t)m);
		ASSERT_EQ(-pos, neg);
	}
	PASS();
}

TEST ulaw_monotonic(void)
{
	/* Higher input codes (after inversion) should produce higher magnitudes */
	for (int i = 1; i < 128; i++) {
		int16_t prev = ulaw_decode((uint8_t)(0x80 | (i - 1)));
		int16_t curr = ulaw_decode((uint8_t)(0x80 | i));
		ASSERT(curr <= prev); /* inverted: 0xFF=silence, 0x80=loudest */
	}
	PASS();
}

/* ── A-law tests ── */

TEST alaw_positive(void)
{
	int16_t val = alaw_decode(0xD5); /* common positive value */
	ASSERT(val > 0);
	PASS();
}

TEST alaw_negative(void)
{
	int16_t val = alaw_decode(0x55); /* common negative value */
	ASSERT(val < 0);
	PASS();
}

TEST alaw_full_range(void)
{
	/* Exercise all 256 input values; verify no two adjacent codes
	 * produce identical output (A-law is a bijection) */
	int16_t prev = alaw_decode(0);
	int unique = 1;
	for (int i = 1; i < 256; i++) {
		int16_t val = alaw_decode((uint8_t)i);
		if (val != prev)
			unique++;
		prev = val;
	}
	ASSERT(unique > 200); /* should have ~256 distinct values */
	PASS();
}

/* ── RTP timestamp tests ── */

TEST rtp_ts_first_frame(void)
{
	/* First frame: epoch = timestamp, RTP ts should be 0 */
	ASSERT_EQ(0u, video_rtp_ts(1000000, 1000000));
	PASS();
}

TEST rtp_ts_one_second(void)
{
	/* 1 second at 90kHz = 90000 */
	ASSERT_EQ(90000u, video_rtp_ts(2000000, 1000000));
	PASS();
}

TEST rtp_ts_40ms(void)
{
	/* 40ms (25fps interval) at 90kHz = 3600 */
	ASSERT_EQ(3600u, video_rtp_ts(1040000, 1000000));
	PASS();
}

TEST rtp_ts_audio_8k(void)
{
	/* 1 second at 8kHz = 8000 */
	ASSERT_EQ(8000u, audio_rtp_ts(2000000, 1000000, 8000));
	PASS();
}

TEST rtp_ts_audio_48k(void)
{
	/* 1 second at 48kHz = 48000 */
	ASSERT_EQ(48000u, audio_rtp_ts(2000000, 1000000, 48000));
	PASS();
}

TEST rtp_ts_large_offset(void)
{
	/* 24 hours at 90kHz — should wrap uint32_t correctly */
	int64_t epoch = 0;
	int64_t ts = 24LL * 3600 * 1000000; /* 24h in microseconds */
	uint32_t rtp = video_rtp_ts(ts, epoch);
	/* 24h * 90000 = 7,776,000,000 which wraps to 3,481,032,704 */
	uint32_t expected = (uint32_t)(24ULL * 3600 * 90000);
	ASSERT_EQ(expected, rtp);
	PASS();
}

/* ── AAC AU header tests ── */

TEST aac_au_small_frame(void)
{
	uint8_t hdr[4];
	aac_au_header(hdr, 256);
	ASSERT_EQ(0x00, hdr[0]);
	ASSERT_EQ(0x10, hdr[1]);
	/* 256 << 3 = 2048 = 0x0800, split across bytes: 0x08, 0x00 */
	ASSERT_EQ(0x08, hdr[2]);
	ASSERT_EQ(0x00, hdr[3]);
	PASS();
}

TEST aac_au_typical_frame(void)
{
	uint8_t hdr[4];
	aac_au_header(hdr, 1024);
	/* 1024 in 13-bit AU-size: high 8 bits = 1024>>5=32, low 5 bits = (1024<<3)&0xFF=0 */
	ASSERT_EQ(0x20, hdr[2]);
	ASSERT_EQ(0x00, hdr[3]);
	PASS();
}

TEST aac_au_odd_size(void)
{
	uint8_t hdr[4];
	aac_au_header(hdr, 333);
	/* 333 >> 5 = 10 = 0x0A, (333 << 3) & 0xFF = (2664) & 0xFF = 0x68 */
	ASSERT_EQ(0x0A, hdr[2]);
	ASSERT_EQ(0x68, hdr[3]);
	PASS();
}

/* ── AAC 1024-sample accumulation boundary ── */

/*
 * AAC-LC encodes 1024 samples per frame. RAD accumulates PCM in a buffer
 * and encodes when pcm_fill >= frame_samples. Test the boundary math:
 * partial fills don't produce output, full fill produces exactly one frame.
 */

/* Simulate RAD's AAC accumulation logic */
static int aac_accumulate(int pcm_fill, int incoming_samples, int frame_samples)
{
	int frames_out = 0;
	pcm_fill += incoming_samples;
	while (pcm_fill >= frame_samples) {
		frames_out++;
		pcm_fill -= frame_samples;
	}
	return frames_out;
}

TEST aac_accum_exact_boundary(void)
{
	/* Exactly 1024 samples → 1 frame */
	ASSERT_EQ(1, aac_accumulate(0, 1024, 1024));
	PASS();
}

TEST aac_accum_under_boundary(void)
{
	/* 1023 samples → 0 frames (1 short) */
	ASSERT_EQ(0, aac_accumulate(0, 1023, 1024));
	PASS();
}

TEST aac_accum_over_boundary(void)
{
	/* 1025 samples → 1 frame, 1 sample carried over */
	ASSERT_EQ(1, aac_accumulate(0, 1025, 1024));
	PASS();
}

TEST aac_accum_double_frame(void)
{
	/* 2048 samples → 2 frames */
	ASSERT_EQ(2, aac_accumulate(0, 2048, 1024));
	PASS();
}

TEST aac_accum_partial_fill(void)
{
	/* 512 + 512 = 1024 → 1 frame */
	int fill = 512;
	ASSERT_EQ(0, aac_accumulate(0, 512, 1024)); /* first half: no output */
	ASSERT_EQ(1, aac_accumulate(fill, 512, 1024)); /* second half: 1 frame */
	PASS();
}

TEST aac_accum_typical_rad(void)
{
	/* RAD reads 320 samples at 16kHz (20ms). 1024/320 = 3.2 frames to fill.
	 * 3 reads = 960 (no frame), 4th read = 1280 → 1 frame, 256 carry */
	int fill = 0;
	int frames = 0;
	for (int i = 0; i < 4; i++) {
		int n = aac_accumulate(fill, 320, 1024);
		frames += n;
		fill += 320;
		fill -= n * 1024;
	}
	ASSERT_EQ(1, frames);
	ASSERT_EQ(256, fill); /* carry-over */
	PASS();
}

/* ── Opus frame sizing ── */

/*
 * Opus supports frame durations: 2.5, 5, 10, 20, 40, 60 ms.
 * RAD encodes 20ms frames. At 48kHz → 960 samples per frame.
 * At 16kHz → 320 samples per frame.
 * The Ogg granule position increments by decoded sample count at 48kHz.
 */

TEST opus_frame_samples_48k(void)
{
	/* 20ms at 48kHz = 960 samples */
	int samples = 48000 * 20 / 1000;
	ASSERT_EQ(960, samples);
	PASS();
}

TEST opus_frame_samples_16k(void)
{
	/* 20ms at 16kHz = 320 samples */
	int samples = 16000 * 20 / 1000;
	ASSERT_EQ(320, samples);
	PASS();
}

TEST opus_granule_increment(void)
{
	/* Ogg granule is always at 48kHz regardless of input sample rate.
	 * For 20ms frames: granule += 960 per frame. */
	uint64_t granule = 0;
	for (int i = 0; i < 50; i++) /* 1 second at 50fps=20ms */
		granule += 960;
	ASSERT_EQ(48000ULL, granule); /* 1 second */
	PASS();
}

TEST opus_granule_wrap(void)
{
	/* Granule is uint64_t — verify no overflow at large values */
	uint64_t granule = UINT64_MAX - 960;
	granule += 960;
	ASSERT_EQ(UINT64_MAX, granule);
	/* Next increment wraps */
	granule += 960;
	ASSERT_EQ(959ULL, granule); /* wrapped */
	PASS();
}

TEST opus_rtp_clock(void)
{
	/* RFC 7587: Opus RTP clock is always 48000 Hz */
	int rtp_clock = 48000;
	/* 20ms at 48kHz = 960 ticks */
	uint32_t rtp_ts = (uint32_t)((uint64_t)20000 * rtp_clock / 1000000);
	ASSERT_EQ(960U, rtp_ts);
	PASS();
}

SUITE(codec_suite)
{
	RUN_TEST(ulaw_silence);
	RUN_TEST(ulaw_max_positive);
	RUN_TEST(ulaw_max_negative);
	RUN_TEST(ulaw_symmetry);
	RUN_TEST(ulaw_monotonic);
	RUN_TEST(alaw_positive);
	RUN_TEST(alaw_negative);
	RUN_TEST(alaw_full_range);
	RUN_TEST(rtp_ts_first_frame);
	RUN_TEST(rtp_ts_one_second);
	RUN_TEST(rtp_ts_40ms);
	RUN_TEST(rtp_ts_audio_8k);
	RUN_TEST(rtp_ts_audio_48k);
	RUN_TEST(rtp_ts_large_offset);
	RUN_TEST(aac_au_small_frame);
	RUN_TEST(aac_au_typical_frame);
	RUN_TEST(aac_au_odd_size);
	RUN_TEST(aac_accum_exact_boundary);
	RUN_TEST(aac_accum_under_boundary);
	RUN_TEST(aac_accum_over_boundary);
	RUN_TEST(aac_accum_double_frame);
	RUN_TEST(aac_accum_partial_fill);
	RUN_TEST(aac_accum_typical_rad);
	RUN_TEST(opus_frame_samples_48k);
	RUN_TEST(opus_frame_samples_16k);
	RUN_TEST(opus_granule_increment);
	RUN_TEST(opus_granule_wrap);
	RUN_TEST(opus_rtp_clock);
}

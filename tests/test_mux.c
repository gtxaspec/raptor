#include "greatest.h"
#include "rmr_mux.h"

#include <stdlib.h>
#include <string.h>

/* ── Memory write callback ── */

static uint8_t *g_buf;
static uint32_t g_len;
static uint32_t g_cap;

static void mux_buf_reset(void)
{
	free(g_buf);
	g_buf = NULL;
	g_len = 0;
	g_cap = 0;
}

static int mem_write(const void *buf, uint32_t len, void *ctx)
{
	(void)ctx;
	if (g_len + len > g_cap) {
		uint32_t new_cap = (g_cap + len) * 2;
		if (new_cap < 65536)
			new_cap = 65536;
		uint8_t *nb = realloc(g_buf, new_cap);
		if (!nb)
			return -1;
		g_buf = nb;
		g_cap = new_cap;
	}
	memcpy(g_buf + g_len, buf, len);
	g_len += len;
	return 0;
}

/* ── Box parsing helpers ── */

static uint32_t rd32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

/* Find a box by 4cc type in [start, start+len). Returns offset or -1. */
static int find_box(const uint8_t *data, uint32_t len, const char *type)
{
	uint32_t off = 0;
	while (off + 8 <= len) {
		uint32_t box_size = rd32(data + off);
		if (box_size < 8 || off + box_size > len)
			break;
		if (memcmp(data + off + 4, type, 4) == 0)
			return (int)off;
		off += box_size;
	}
	return -1;
}

/* Find a box inside a container box (skip container's 8-byte header) */
static int find_child_box(const uint8_t *data, uint32_t len, uint32_t parent_off,
			  const char *type)
{
	uint32_t parent_size = rd32(data + parent_off);
	if (parent_off + parent_size > len)
		return -1;
	uint32_t payload_off = parent_off + 8;
	uint32_t payload_len = parent_size - 8;
	int child = find_box(data + payload_off, payload_len, type);
	return child >= 0 ? (int)(payload_off + (uint32_t)child) : -1;
}

/* Test data */
static const uint8_t test_sps[] = {0x67, 0x42, 0xC0, 0x1E, 0xD9, 0x00, 0xA0, 0x47, 0xFE, 0xC8};
static const uint8_t test_pps[] = {0x68, 0xCE, 0x38, 0x80};

static uint8_t *make_fake_nal(uint32_t payload_size, uint8_t nal_type, uint32_t *out_size)
{
	*out_size = 4 + payload_size;
	uint8_t *buf = malloc(*out_size);
	if (!buf)
		return NULL;
	buf[0] = (payload_size >> 24) & 0xFF;
	buf[1] = (payload_size >> 16) & 0xFF;
	buf[2] = (payload_size >> 8) & 0xFF;
	buf[3] = payload_size & 0xFF;
	buf[4] = nal_type;
	memset(buf + 5, 0xAB, payload_size - 1);
	return buf;
}

/* ── Tests ── */

TEST mux_video_h264(void)
{
	mux_buf_reset();
	rmr_mux_t *mux = rmr_mux_create(mem_write, NULL);
	ASSERT(mux);

	rmr_video_params_t vp = {.codec = RMR_CODEC_H264, .width = 640, .height = 480, .timescale = 90000};
	rmr_mux_set_video(mux, &vp, test_sps, sizeof(test_sps), test_pps, sizeof(test_pps), NULL, 0);
	rmr_mux_start(mux);

	/* One GOP: IDR + 4 P-frames */
	for (int i = 0; i < 5; i++) {
		bool key = (i == 0);
		uint32_t ns;
		uint8_t *nal = make_fake_nal(key ? 2048 : 512, key ? 0x65 : 0x41, &ns);
		rmr_video_sample_t vs = {.data = nal, .size = ns, .dts = i * 3600,
					 .pts = i * 3600, .is_key = key};
		rmr_mux_write_video(mux, &vs);
		free(nal);
	}
	rmr_mux_flush_fragment(mux);
	rmr_mux_destroy(mux);

	/* Verify boxes */
	ASSERT(g_len > 0);
	int ftyp = find_box(g_buf, g_len, "ftyp");
	ASSERT(ftyp >= 0);
	/* ftyp should contain "isom" brand */
	ASSERT(memmem(g_buf + ftyp, rd32(g_buf + ftyp), "isom", 4));

	int moov = find_box(g_buf, g_len, "moov");
	ASSERT(moov >= 0);

	int moof = find_box(g_buf, g_len, "moof");
	ASSERT(moof >= 0);

	int mdat = find_box(g_buf, g_len, "mdat");
	ASSERT(mdat >= 0);

	mux_buf_reset();
	PASS();
}

TEST mux_video_h265(void)
{
	mux_buf_reset();
	rmr_mux_t *mux = rmr_mux_create(mem_write, NULL);
	ASSERT(mux);

	/* H.265 VPS/SPS/PPS (minimal test data) */
	uint8_t vps[] = {0x40, 0x01, 0x0C, 0x01};
	uint8_t sps265[] = {0x42, 0x01, 0x01, 0x01};
	uint8_t pps265[] = {0x44, 0x01, 0xC0};

	rmr_video_params_t vp = {.codec = RMR_CODEC_H265, .width = 1920, .height = 1080, .timescale = 90000};
	rmr_mux_set_video(mux, &vp, sps265, sizeof(sps265), pps265, sizeof(pps265), vps, sizeof(vps));
	rmr_mux_start(mux);

	/* One keyframe */
	uint32_t ns;
	uint8_t *nal = make_fake_nal(1024, 0x26, &ns); /* IDR NAL for H.265 */
	rmr_video_sample_t vs = {.data = nal, .size = ns, .dts = 0, .pts = 0, .is_key = true};
	rmr_mux_write_video(mux, &vs);
	free(nal);
	rmr_mux_flush_fragment(mux);
	rmr_mux_destroy(mux);

	/* Verify moov contains hev1 or hvc1 box (H.265 codec) */
	int moov = find_box(g_buf, g_len, "moov");
	ASSERT(moov >= 0);
	/* Look for hvcC configuration box somewhere in the output */
	ASSERT(memmem(g_buf, g_len, "hvcC", 4) || memmem(g_buf, g_len, "hev1", 4));

	mux_buf_reset();
	PASS();
}

TEST mux_audio_pcmu(void)
{
	mux_buf_reset();
	rmr_mux_t *mux = rmr_mux_create(mem_write, NULL);
	ASSERT(mux);

	rmr_audio_params_t ap = {.codec = RMR_AUDIO_PCMU, .sample_rate = 8000, .channels = 1, .bits_per_sample = 8};
	rmr_mux_set_audio(mux, &ap);
	rmr_mux_start(mux);

	/* Push audio samples */
	uint8_t audio[160];
	memset(audio, 0x80, sizeof(audio));
	for (int i = 0; i < 10; i++) {
		rmr_audio_sample_t as = {.data = audio, .size = sizeof(audio), .dts = i * 160};
		rmr_mux_write_audio(mux, &as);
	}
	rmr_mux_flush_fragment(mux);
	rmr_mux_destroy(mux);

	ASSERT(g_len > 0);
	ASSERT(find_box(g_buf, g_len, "moov") >= 0);
	ASSERT(find_box(g_buf, g_len, "moof") >= 0);
	ASSERT(find_box(g_buf, g_len, "mdat") >= 0);

	mux_buf_reset();
	PASS();
}

TEST mux_combined(void)
{
	mux_buf_reset();
	rmr_mux_t *mux = rmr_mux_create(mem_write, NULL);
	ASSERT(mux);

	rmr_video_params_t vp = {.codec = RMR_CODEC_H264, .width = 640, .height = 480, .timescale = 90000};
	rmr_audio_params_t ap = {.codec = RMR_AUDIO_L16, .sample_rate = 16000, .channels = 1, .bits_per_sample = 16};
	rmr_mux_set_video(mux, &vp, test_sps, sizeof(test_sps), test_pps, sizeof(test_pps), NULL, 0);
	rmr_mux_set_audio(mux, &ap);
	rmr_mux_start(mux);

	/* 1 keyframe + audio */
	uint32_t ns;
	uint8_t *nal = make_fake_nal(1024, 0x65, &ns);
	rmr_video_sample_t vs = {.data = nal, .size = ns, .dts = 0, .pts = 0, .is_key = true};
	rmr_mux_write_video(mux, &vs);
	free(nal);

	uint8_t audio[320];
	memset(audio, 0x80, sizeof(audio));
	rmr_audio_sample_t as = {.data = audio, .size = sizeof(audio), .dts = 0};
	rmr_mux_write_audio(mux, &as);

	rmr_mux_flush_fragment(mux);
	rmr_mux_destroy(mux);

	ASSERT(g_len > 0);
	int moov = find_box(g_buf, g_len, "moov");
	ASSERT(moov >= 0);

	/* moov should contain mvhd + at least 2 trak boxes */
	int trak1 = find_child_box(g_buf, g_len, (uint32_t)moov, "trak");
	ASSERT(trak1 >= 0);
	/* Find second trak after first */
	uint32_t first_trak_size = rd32(g_buf + trak1);
	uint32_t after_trak1 = (uint32_t)trak1 + first_trak_size;
	uint32_t moov_end = (uint32_t)moov + rd32(g_buf + moov);
	int trak2 = find_box(g_buf + after_trak1, moov_end - after_trak1, "trak");
	ASSERT(trak2 >= 0);

	mux_buf_reset();
	PASS();
}

TEST mux_multi_fragment(void)
{
	mux_buf_reset();
	rmr_mux_t *mux = rmr_mux_create(mem_write, NULL);
	ASSERT(mux);

	rmr_video_params_t vp = {.codec = RMR_CODEC_H264, .width = 640, .height = 480, .timescale = 90000};
	rmr_mux_set_video(mux, &vp, test_sps, sizeof(test_sps), test_pps, sizeof(test_pps), NULL, 0);
	rmr_mux_start(mux);

	int64_t dts = 0;
	for (int gop = 0; gop < 3; gop++) {
		for (int f = 0; f < 5; f++) {
			bool key = (f == 0);
			uint32_t ns;
			uint8_t *nal = make_fake_nal(key ? 1024 : 256, key ? 0x65 : 0x41, &ns);
			rmr_video_sample_t vs = {.data = nal, .size = ns, .dts = dts,
						 .pts = dts, .is_key = key};
			rmr_mux_write_video(mux, &vs);
			free(nal);
			dts += 3600;
		}
		rmr_mux_flush_fragment(mux);
	}
	rmr_mux_destroy(mux);

	/* Count moof boxes — should be 3 */
	int moof_count = 0;
	uint32_t off = 0;
	while (off + 8 <= g_len) {
		uint32_t box_size = rd32(g_buf + off);
		if (box_size < 8 || off + box_size > g_len)
			break;
		if (memcmp(g_buf + off + 4, "moof", 4) == 0)
			moof_count++;
		off += box_size;
	}
	ASSERT_EQ(3, moof_count);

	mux_buf_reset();
	PASS();
}

TEST mux_verify_avcC(void)
{
	mux_buf_reset();
	rmr_mux_t *mux = rmr_mux_create(mem_write, NULL);
	ASSERT(mux);

	rmr_video_params_t vp = {.codec = RMR_CODEC_H264, .width = 640, .height = 480, .timescale = 90000};
	rmr_mux_set_video(mux, &vp, test_sps, sizeof(test_sps), test_pps, sizeof(test_pps), NULL, 0);
	rmr_mux_start(mux);
	rmr_mux_destroy(mux);

	/* Just the init segment (ftyp + moov), check for avcC box containing SPS/PPS */
	ASSERT(g_len > 0);
	void *avcc = memmem(g_buf, g_len, "avcC", 4);
	ASSERT(avcc);
	/* SPS data should appear in the output after avcC */
	ASSERT(memmem(g_buf, g_len, test_sps, sizeof(test_sps)));
	/* PPS data should appear too */
	ASSERT(memmem(g_buf, g_len, test_pps, sizeof(test_pps)));

	mux_buf_reset();
	PASS();
}

TEST mux_finalize(void)
{
	mux_buf_reset();
	rmr_mux_t *mux = rmr_mux_create(mem_write, NULL);
	ASSERT(mux);

	rmr_video_params_t vp = {.codec = RMR_CODEC_H264, .width = 640, .height = 480, .timescale = 90000};
	rmr_mux_set_video(mux, &vp, test_sps, sizeof(test_sps), test_pps, sizeof(test_pps), NULL, 0);
	rmr_mux_start(mux);

	uint32_t ns;
	uint8_t *nal = make_fake_nal(512, 0x65, &ns);
	rmr_video_sample_t vs = {.data = nal, .size = ns, .dts = 0, .pts = 0, .is_key = true};
	rmr_mux_write_video(mux, &vs);
	free(nal);
	rmr_mux_flush_fragment(mux);

	uint32_t pre_finalize = g_len;
	rmr_mux_finalize(mux);
	/* finalize should write something (mfra box) */
	ASSERT(g_len > pre_finalize);
	ASSERT(find_box(g_buf, g_len, "mfra") >= 0);

	rmr_mux_destroy(mux);
	mux_buf_reset();
	PASS();
}

/* ── Edge-case tests ── */

TEST mux_empty_flush(void)
{
	mux_buf_reset();
	rmr_mux_t *mux = rmr_mux_create(mem_write, NULL);
	ASSERT(mux);

	rmr_video_params_t vp = {.codec = RMR_CODEC_H264, .width = 640, .height = 480, .timescale = 90000};
	rmr_mux_set_video(mux, &vp, test_sps, sizeof(test_sps), test_pps, sizeof(test_pps), NULL, 0);
	rmr_mux_start(mux);

	uint32_t after_init = g_len;
	/* Flush with zero samples — should not crash, should not write anything */
	int ret = rmr_mux_flush_fragment(mux);
	ASSERT_EQ(0, ret);
	ASSERT_EQ(after_init, g_len); /* no new data */

	rmr_mux_destroy(mux);
	mux_buf_reset();
	PASS();
}

TEST mux_double_flush(void)
{
	mux_buf_reset();
	rmr_mux_t *mux = rmr_mux_create(mem_write, NULL);
	ASSERT(mux);

	rmr_video_params_t vp = {.codec = RMR_CODEC_H264, .width = 640, .height = 480, .timescale = 90000};
	rmr_mux_set_video(mux, &vp, test_sps, sizeof(test_sps), test_pps, sizeof(test_pps), NULL, 0);
	rmr_mux_start(mux);

	uint32_t ns;
	uint8_t *nal = make_fake_nal(256, 0x65, &ns);
	rmr_video_sample_t vs = {.data = nal, .size = ns, .dts = 0, .pts = 0, .is_key = true};
	rmr_mux_write_video(mux, &vs);
	free(nal);

	ASSERT_EQ(0, rmr_mux_flush_fragment(mux));
	uint32_t after_first_flush = g_len;

	/* Second flush with no new data — should be harmless */
	ASSERT_EQ(0, rmr_mux_flush_fragment(mux));
	ASSERT_EQ(after_first_flush, g_len);

	rmr_mux_destroy(mux);
	mux_buf_reset();
	PASS();
}

TEST mux_start_no_tracks(void)
{
	mux_buf_reset();
	rmr_mux_t *mux = rmr_mux_create(mem_write, NULL);
	ASSERT(mux);

	/* Start without setting any tracks — should fail */
	int ret = rmr_mux_start(mux);
	ASSERT_EQ(-1, ret);
	ASSERT_EQ(0u, g_len); /* nothing written */

	rmr_mux_destroy(mux);
	mux_buf_reset();
	PASS();
}

TEST mux_single_sample_fragment(void)
{
	mux_buf_reset();
	rmr_mux_t *mux = rmr_mux_create(mem_write, NULL);
	ASSERT(mux);

	rmr_video_params_t vp = {.codec = RMR_CODEC_H264, .width = 640, .height = 480, .timescale = 90000};
	rmr_mux_set_video(mux, &vp, test_sps, sizeof(test_sps), test_pps, sizeof(test_pps), NULL, 0);
	rmr_mux_start(mux);

	/* Single keyframe, immediate flush */
	uint32_t ns;
	uint8_t *nal = make_fake_nal(512, 0x65, &ns);
	rmr_video_sample_t vs = {.data = nal, .size = ns, .dts = 0, .pts = 0, .is_key = true};
	rmr_mux_write_video(mux, &vs);
	free(nal);

	ASSERT_EQ(0, rmr_mux_flush_fragment(mux));

	/* Should produce valid ftyp + moov + moof + mdat */
	ASSERT(find_box(g_buf, g_len, "ftyp") >= 0);
	ASSERT(find_box(g_buf, g_len, "moov") >= 0);
	ASSERT(find_box(g_buf, g_len, "moof") >= 0);
	ASSERT(find_box(g_buf, g_len, "mdat") >= 0);

	rmr_mux_destroy(mux);
	mux_buf_reset();
	PASS();
}

TEST mux_large_timestamps(void)
{
	mux_buf_reset();
	rmr_mux_t *mux = rmr_mux_create(mem_write, NULL);
	ASSERT(mux);

	rmr_video_params_t vp = {.codec = RMR_CODEC_H264, .width = 640, .height = 480, .timescale = 90000};
	rmr_mux_set_video(mux, &vp, test_sps, sizeof(test_sps), test_pps, sizeof(test_pps), NULL, 0);
	rmr_mux_start(mux);

	/* Simulate 24 hours of recording at 90kHz timescale.
	 * 24h = 86400s * 90000 = 7,776,000,000 — exceeds uint32 max.
	 * The muxer uses int64_t for DTS and 64-bit tfdt, so this should work. */
	int64_t base_dts = 7776000000LL;
	for (int i = 0; i < 5; i++) {
		bool key = (i == 0);
		uint32_t ns;
		uint8_t *nal = make_fake_nal(key ? 1024 : 256, key ? 0x65 : 0x41, &ns);
		rmr_video_sample_t vs = {.data = nal, .size = ns, .dts = base_dts + i * 3600,
					 .pts = base_dts + i * 3600, .is_key = key};
		rmr_mux_write_video(mux, &vs);
		free(nal);
	}
	ASSERT_EQ(0, rmr_mux_flush_fragment(mux));

	/* Should not crash and should produce valid boxes */
	ASSERT(find_box(g_buf, g_len, "moof") >= 0);
	ASSERT(find_box(g_buf, g_len, "mdat") >= 0);

	/* Verify tfdt contains 64-bit timestamp: look for tfdt box,
	 * version byte should be 1 (64-bit decode time) */
	void *tfdt = memmem(g_buf, g_len, "tfdt", 4);
	ASSERT(tfdt);
	/* tfdt box: [size:4][type:4][version:1][flags:3][base_media_decode_time:8]
	 * version byte is right after the "tfdt" fourcc */
	uint8_t *tfdt_ver = (uint8_t *)tfdt + 4;
	ASSERT_EQ(1, *tfdt_ver); /* version 1 = 64-bit timestamps */

	rmr_mux_destroy(mux);
	mux_buf_reset();
	PASS();
}

TEST mux_cts_offset(void)
{
	mux_buf_reset();
	rmr_mux_t *mux = rmr_mux_create(mem_write, NULL);
	ASSERT(mux);

	rmr_video_params_t vp = {.codec = RMR_CODEC_H264, .width = 640, .height = 480, .timescale = 90000};
	rmr_mux_set_video(mux, &vp, test_sps, sizeof(test_sps), test_pps, sizeof(test_pps), NULL, 0);
	rmr_mux_start(mux);

	/* PTS != DTS: simulate reordering (PTS ahead of DTS by 2 frames) */
	uint32_t ns;
	uint8_t *nal = make_fake_nal(512, 0x65, &ns);
	rmr_video_sample_t vs = {.data = nal, .size = ns, .dts = 0, .pts = 7200, .is_key = true};
	rmr_mux_write_video(mux, &vs);
	free(nal);

	nal = make_fake_nal(256, 0x41, &ns);
	rmr_video_sample_t vs2 = {.data = nal, .size = ns, .dts = 3600, .pts = 3600, .is_key = false};
	rmr_mux_write_video(mux, &vs2);
	free(nal);

	ASSERT_EQ(0, rmr_mux_flush_fragment(mux));

	/* Verify trun has composition_time_offsets flag (0x000800) */
	void *trun = memmem(g_buf, g_len, "trun", 4);
	ASSERT(trun);
	/* trun box: [size:4]["trun":4][version:1][flags:3] */
	uint8_t *trun_flags = (uint8_t *)trun + 4 + 1; /* skip version byte */
	uint32_t flags = ((uint32_t)trun_flags[0] << 16) |
			 ((uint32_t)trun_flags[1] << 8) | trun_flags[2];
	ASSERT(flags & 0x000800); /* sample_composition_time_offsets_present */

	rmr_mux_destroy(mux);
	mux_buf_reset();
	PASS();
}

TEST mux_null_inputs(void)
{
	/* NULL write function */
	ASSERT_EQ(NULL, rmr_mux_create(NULL, NULL));

	/* NULL mux */
	ASSERT_EQ(-1, rmr_mux_start(NULL));
	ASSERT_EQ(-1, rmr_mux_flush_fragment(NULL));
	ASSERT_EQ(-1, rmr_mux_finalize(NULL));

	/* set_video with missing params */
	rmr_mux_t *mux = rmr_mux_create(mem_write, NULL);
	ASSERT(mux);
	ASSERT_EQ(-1, rmr_mux_set_video(mux, NULL, test_sps, sizeof(test_sps),
					 test_pps, sizeof(test_pps), NULL, 0));

	/* write_video with NULL sample */
	ASSERT_EQ(-1, rmr_mux_write_video(mux, NULL));

	/* write_video with zero-size sample */
	rmr_video_sample_t vs = {.data = test_sps, .size = 0, .dts = 0, .pts = 0, .is_key = true};
	ASSERT_EQ(-1, rmr_mux_write_video(mux, &vs));

	rmr_mux_destroy(mux);
	PASS();
}

TEST mux_audio_aac(void)
{
	mux_buf_reset();
	rmr_mux_t *mux = rmr_mux_create(mem_write, NULL);
	ASSERT(mux);

	rmr_audio_params_t ap = {.codec = RMR_AUDIO_AAC, .sample_rate = 16000, .channels = 1, .bits_per_sample = 16};
	rmr_mux_set_audio(mux, &ap);
	rmr_mux_start(mux);

	/* Push AAC frames (1024 samples per frame at 16kHz = 64ms) */
	uint8_t aac_frame[128];
	memset(aac_frame, 0x21, sizeof(aac_frame));
	for (int i = 0; i < 10; i++) {
		rmr_audio_sample_t as = {.data = aac_frame, .size = sizeof(aac_frame), .dts = i * 1024};
		rmr_mux_write_audio(mux, &as);
	}
	ASSERT_EQ(0, rmr_mux_flush_fragment(mux));

	/* Verify mp4a + esds boxes exist (AAC-specific) */
	ASSERT(memmem(g_buf, g_len, "mp4a", 4));
	ASSERT(memmem(g_buf, g_len, "esds", 4));

	/* esds should NOT be present for non-AAC codecs — verify by absence of
	 * "ulaw" or "Opus" boxes */
	ASSERT_FALSE(memmem(g_buf, g_len, "ulaw", 4));
	ASSERT_FALSE(memmem(g_buf, g_len, "dOps", 4));

	rmr_mux_destroy(mux);
	mux_buf_reset();
	PASS();
}

TEST mux_audio_opus(void)
{
	mux_buf_reset();
	rmr_mux_t *mux = rmr_mux_create(mem_write, NULL);
	ASSERT(mux);

	rmr_audio_params_t ap = {.codec = RMR_AUDIO_OPUS, .sample_rate = 16000, .channels = 1, .bits_per_sample = 16};
	rmr_mux_set_audio(mux, &ap);
	rmr_mux_start(mux);

	/* Push Opus frames (20ms at 16kHz = 320 samples) */
	uint8_t opus_frame[64];
	memset(opus_frame, 0x42, sizeof(opus_frame));
	for (int i = 0; i < 10; i++) {
		rmr_audio_sample_t as = {.data = opus_frame, .size = sizeof(opus_frame), .dts = i * 320};
		rmr_mux_write_audio(mux, &as);
	}
	ASSERT_EQ(0, rmr_mux_flush_fragment(mux));

	/* Verify Opus + dOps boxes exist */
	ASSERT(memmem(g_buf, g_len, "Opus", 4));
	ASSERT(memmem(g_buf, g_len, "dOps", 4));

	/* Should NOT have AAC or PCM boxes */
	ASSERT_FALSE(memmem(g_buf, g_len, "mp4a", 4));
	ASSERT_FALSE(memmem(g_buf, g_len, "esds", 4));

	rmr_mux_destroy(mux);
	mux_buf_reset();
	PASS();
}

TEST mux_av_duration(void)
{
	/* Real-world scenario: 25fps video (3600 ticks at 90kHz) interleaved
	 * with 50fps audio (160 samples at 8kHz = 20ms frames).
	 * Two audio frames per video frame. Verify both tracks produce
	 * valid fragments with correct sample counts and mdat sizes. */
	mux_buf_reset();
	rmr_mux_t *mux = rmr_mux_create(mem_write, NULL);
	ASSERT(mux);

	rmr_video_params_t vp = {.codec = RMR_CODEC_H264, .width = 640, .height = 480, .timescale = 90000};
	rmr_audio_params_t ap = {.codec = RMR_AUDIO_PCMU, .sample_rate = 8000, .channels = 1, .bits_per_sample = 8};
	rmr_mux_set_video(mux, &vp, test_sps, sizeof(test_sps), test_pps, sizeof(test_pps), NULL, 0);
	rmr_mux_set_audio(mux, &ap);
	rmr_mux_start(mux);

	int64_t v_dts = 0;
	int64_t a_dts = 0;
	uint32_t total_v_data = 0;
	uint32_t total_a_data = 0;

	/* 1 second: 25 video frames + 50 audio frames */
	for (int f = 0; f < 25; f++) {
		bool key = (f == 0);
		uint32_t ns;
		uint8_t *nal = make_fake_nal(key ? 4096 : 512, key ? 0x65 : 0x41, &ns);
		rmr_video_sample_t vs = {.data = nal, .size = ns, .dts = v_dts, .pts = v_dts, .is_key = key};
		rmr_mux_write_video(mux, &vs);
		total_v_data += ns;
		free(nal);
		v_dts += 3600;

		/* 2 audio frames per video frame */
		for (int a = 0; a < 2; a++) {
			uint8_t audio[160]; /* 20ms at 8kHz, 8-bit */
			memset(audio, 0x80, sizeof(audio));
			rmr_audio_sample_t as = {.data = audio, .size = sizeof(audio), .dts = a_dts};
			rmr_mux_write_audio(mux, &as);
			total_a_data += sizeof(audio);
			a_dts += 160;
		}
	}
	ASSERT_EQ(0, rmr_mux_flush_fragment(mux));

	/* Verify structure */
	ASSERT(find_box(g_buf, g_len, "moof") >= 0);

	int mdat_off = find_box(g_buf, g_len, "mdat");
	ASSERT(mdat_off >= 0);
	uint32_t mdat_size = rd32(g_buf + mdat_off);
	/* mdat should contain all video + audio data + 8 byte header */
	ASSERT_EQ(8 + total_v_data + total_a_data, mdat_size);

	/* Count trun boxes — should be 2 (one video, one audio) */
	int trun_count = 0;
	for (uint32_t i = 0; i + 4 <= g_len; i++) {
		if (memcmp(g_buf + i, "trun", 4) == 0)
			trun_count++;
	}
	ASSERT_EQ(2, trun_count);

	/* Find the two trun boxes and verify sample counts.
	 * trun format: [size:4]["trun":4][ver:1][flags:3][sample_count:4]... */
	int truns_found = 0;
	for (uint32_t i = 0; i + 16 <= g_len; i++) {
		if (memcmp(g_buf + i + 4, "trun", 4) == 0) {
			uint32_t sample_count = rd32(g_buf + i + 12);
			if (truns_found == 0)
				ASSERT_EQ(25u, sample_count); /* video: 25 frames */
			else
				ASSERT_EQ(50u, sample_count); /* audio: 50 frames */
			truns_found++;
		}
	}
	ASSERT_EQ(2, truns_found);

	rmr_mux_destroy(mux);
	mux_buf_reset();
	PASS();
}

SUITE(mux_suite)
{
	RUN_TEST(mux_video_h264);
	RUN_TEST(mux_video_h265);
	RUN_TEST(mux_audio_pcmu);
	RUN_TEST(mux_combined);
	RUN_TEST(mux_multi_fragment);
	RUN_TEST(mux_verify_avcC);
	RUN_TEST(mux_finalize);
	RUN_TEST(mux_empty_flush);
	RUN_TEST(mux_double_flush);
	RUN_TEST(mux_start_no_tracks);
	RUN_TEST(mux_single_sample_fragment);
	RUN_TEST(mux_large_timestamps);
	RUN_TEST(mux_cts_offset);
	RUN_TEST(mux_null_inputs);
	RUN_TEST(mux_audio_aac);
	RUN_TEST(mux_audio_opus);
	RUN_TEST(mux_av_duration);
}

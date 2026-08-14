/*
 * test_backchannel.c -- backchannel decode: G.711 expansion, L16 byte
 * order, and the per-PT dispatch policy (decode known payload types,
 * drop unknown ones, never publish raw bytes into the PCM ring).
 */

#include "greatest.h"

#include <errno.h>
#include <string.h>

#include <rss_ipc.h>

#include "../rsd/rsd_backchannel.h"

/* Golden vectors from the G.711 expansion tables. */
TEST bc_ulaw_golden(void)
{
	ASSERT_EQ(0, rsd_bc_ulaw_decode(0xFF));	     /* positive zero */
	ASSERT_EQ(0, rsd_bc_ulaw_decode(0x7F));	     /* negative zero */
	ASSERT_EQ(-32124, rsd_bc_ulaw_decode(0x00)); /* max negative */
	ASSERT_EQ(32124, rsd_bc_ulaw_decode(0x80));  /* max positive */
	PASS();
}

TEST bc_alaw_golden(void)
{
	ASSERT_EQ(8, rsd_bc_alaw_decode(0xD5));	     /* smallest positive */
	ASSERT_EQ(-8, rsd_bc_alaw_decode(0x55));     /* smallest negative */
	ASSERT_EQ(32256, rsd_bc_alaw_decode(0xAA));  /* max positive */
	ASSERT_EQ(-32256, rsd_bc_alaw_decode(0x2A)); /* max negative */
	PASS();
}

/* Bit 7 of the transmitted byte carries the sign in both laws (with
 * opposite polarity, which the golden vectors above pin), so flipping
 * it must exactly negate every decoded sample. */
TEST bc_g711_sign_symmetry(void)
{
	for (int v = 0; v < 256; v++) {
		ASSERT_EQ(rsd_bc_ulaw_decode((uint8_t)v),
			  (int16_t)-rsd_bc_ulaw_decode((uint8_t)(v ^ 0x80)));
		ASSERT_EQ(rsd_bc_alaw_decode((uint8_t)v),
			  (int16_t)-rsd_bc_alaw_decode((uint8_t)(v ^ 0x80)));
	}
	PASS();
}

/* One writer + one reader around a dispatch call; returns the read rc. */
static int bc_feed_and_read(const char *ring_name, uint8_t pt, const uint8_t *payload, size_t len,
			    uint8_t *out, uint32_t out_cap, uint32_t *out_len,
			    uint32_t *unknown_count)
{
	rss_ring_t *w = rss_ring_create(ring_name, 4, 8192);
	if (!w)
		return -1;
	rss_ring_t *rd = rss_ring_open(ring_name);
	if (!rd) {
		rss_ring_destroy(w);
		return -1;
	}

	rsd_bc_dec_t dec;
	rsd_bc_dec_init(&dec);
	rsd_bc_handle(&dec, w, pt, 0, payload, len);
	if (unknown_count)
		*unknown_count = dec.unknown_pt_count;
	rsd_bc_dec_deinit(&dec);

	uint64_t seq = 1;
	rss_ring_slot_t meta;
	int rc = rss_ring_read(rd, &seq, out, out_cap, out_len, &meta);

	rss_ring_close(rd);
	rss_ring_destroy(w);
	return rc;
}

TEST bc_dispatch_pcmu_publishes_doubled_pcm(void)
{
	uint8_t in[160];
	memset(in, 0xFF, sizeof(in)); /* mu-law silence */
	uint8_t out[4096];
	uint32_t len = 0;

	ASSERT_EQ(0,
		  bc_feed_and_read("test_bc_u", 0, in, sizeof(in), out, sizeof(out), &len, NULL));
	ASSERT_EQ(160 * 4, (int)len); /* 8k -> 16k doubling, 2 bytes/sample */
	for (uint32_t i = 0; i < len; i++)
		ASSERT_EQ(0, out[i]);
	PASS();
}

TEST bc_dispatch_pcma_publishes_doubled_pcm(void)
{
	uint8_t in[160];
	memset(in, 0xD5, sizeof(in)); /* A-law +8 */
	uint8_t out[4096];
	uint32_t len = 0;

	ASSERT_EQ(0,
		  bc_feed_and_read("test_bc_a", 8, in, sizeof(in), out, sizeof(out), &len, NULL));
	ASSERT_EQ(160 * 4, (int)len);
	const int16_t *pcm = (const int16_t *)out;
	for (uint32_t i = 0; i < len / 2; i++)
		ASSERT_EQ(8, pcm[i]);
	PASS();
}

TEST bc_dispatch_l16_swaps_network_order(void)
{
	const uint8_t in[] = {0x12, 0x34, 0x80, 0x00, 0xFF}; /* odd tail byte drops */
	uint8_t out[64];
	uint32_t len = 0;

	ASSERT_EQ(0, bc_feed_and_read("test_bc_l", RSD_BC_PT_L16, in, sizeof(in), out, sizeof(out),
				      &len, NULL));
	ASSERT_EQ(4, (int)len);
	const int16_t *pcm = (const int16_t *)out;
	ASSERT_EQ(0x1234, pcm[0]);
	ASSERT_EQ(-32768, pcm[1]);
	PASS();
}

TEST bc_dispatch_unknown_pt_drops(void)
{
	const uint8_t in[] = {1, 2, 3, 4};
	uint8_t out[64];
	uint32_t len = 0;
	uint32_t unknown = 0;

	int rc =
		bc_feed_and_read("test_bc_x", 99, in, sizeof(in), out, sizeof(out), &len, &unknown);
	ASSERT_EQ(-EAGAIN, rc); /* nothing published */
	ASSERT_EQ(1, (int)unknown);
	PASS();
}

TEST bc_dispatch_empty_payload_is_noop(void)
{
	uint8_t out[64];
	uint32_t len = 0;

	ASSERT_EQ(-EAGAIN, bc_feed_and_read("test_bc_e", 0, NULL, 0, out, sizeof(out), &len, NULL));
	PASS();
}

/* Build an RFC 3640 AAC-hbr payload: AU-headers-length (bits), then
 * 16-bit headers size(13)|index(3), then the AU bytes. */
static size_t aac_hbr_build(uint8_t *out, size_t cap, const size_t *sizes, const uint8_t *idx3,
			    int n_aus, size_t data_bytes)
{
	size_t need = 2 + (size_t)n_aus * 2 + data_bytes;
	if (need > cap)
		return 0;
	uint16_t hdr_bits = (uint16_t)(n_aus * 16);
	out[0] = (uint8_t)(hdr_bits >> 8);
	out[1] = (uint8_t)hdr_bits;
	for (int i = 0; i < n_aus; i++) {
		uint16_t h = (uint16_t)((sizes[i] << 3) | (idx3 ? idx3[i] : 0));
		out[2 + i * 2] = (uint8_t)(h >> 8);
		out[3 + i * 2] = (uint8_t)h;
	}
	for (size_t i = 0; i < data_bytes; i++)
		out[2 + (size_t)n_aus * 2 + i] = (uint8_t)i;
	return need;
}

TEST bc_aac_parse_single_au(void)
{
	uint8_t buf[64];
	const size_t sizes[] = {5};
	size_t len = aac_hbr_build(buf, sizeof(buf), sizes, NULL, 1, 5);
	ASSERT(len > 0);

	rsd_bc_au_t aus[4];
	size_t frag = 0;
	ASSERT_EQ(1, rsd_bc_aac_parse(buf, len, aus, 4, &frag));
	ASSERT_EQ(5, (int)aus[0].len);
	ASSERT_EQ(0, memcmp(aus[0].ptr, "\x00\x01\x02\x03\x04", 5));
	PASS();
}

TEST bc_aac_parse_multi_au(void)
{
	uint8_t buf[64];
	const size_t sizes[] = {3, 4};
	size_t len = aac_hbr_build(buf, sizeof(buf), sizes, NULL, 2, 7);
	ASSERT(len > 0);

	rsd_bc_au_t aus[4];
	size_t frag = 0;
	ASSERT_EQ(2, rsd_bc_aac_parse(buf, len, aus, 4, &frag));
	ASSERT_EQ(3, (int)aus[0].len);
	ASSERT_EQ(4, (int)aus[1].len);
	ASSERT_EQ(3, (int)(aus[1].ptr - aus[0].ptr));
	PASS();
}

TEST bc_aac_parse_fragment(void)
{
	uint8_t buf[64];
	const size_t sizes[] = {600}; /* header names the FULL AU size */
	size_t len = aac_hbr_build(buf, sizeof(buf), sizes, NULL, 1, 40);
	ASSERT(len > 0);

	rsd_bc_au_t aus[4];
	size_t frag = 0;
	ASSERT_EQ(0, rsd_bc_aac_parse(buf, len, aus, 4, &frag));
	ASSERT_EQ(600, (int)frag);
	ASSERT_EQ(40, (int)aus[0].len);
	PASS();
}

TEST bc_aac_parse_malformed(void)
{
	rsd_bc_au_t aus[4];
	size_t frag = 0;
	uint8_t buf[64];

	/* Too short for even the headers-length field. */
	ASSERT_EQ(-1, rsd_bc_aac_parse((const uint8_t *)"\x00", 1, aus, 4, &frag));

	/* Headers-length not a multiple of one 16-bit header. */
	buf[0] = 0;
	buf[1] = 8;
	buf[2] = buf[3] = 0;
	ASSERT_EQ(-1, rsd_bc_aac_parse(buf, 4, aus, 4, &frag));

	/* Nonzero index: interleaving was never offered. */
	const size_t sizes[] = {4};
	const uint8_t idx[] = {1};
	size_t len = aac_hbr_build(buf, sizeof(buf), sizes, idx, 1, 4);
	ASSERT_EQ(-1, rsd_bc_aac_parse(buf, len, aus, 4, &frag));

	/* Trailing bytes the headers never named. */
	len = aac_hbr_build(buf, sizeof(buf), sizes, NULL, 1, 6);
	ASSERT_EQ(-1, rsd_bc_aac_parse(buf, len, aus, 4, &frag));

	/* Short data across TWO AUs is malformed, not a fragment. */
	const size_t two[] = {5, 5};
	len = aac_hbr_build(buf, sizeof(buf), two, NULL, 2, 7);
	ASSERT_EQ(-1, rsd_bc_aac_parse(buf, len, aus, 4, &frag));
	PASS();
}

SUITE(backchannel_suite)
{
	RUN_TEST(bc_ulaw_golden);
	RUN_TEST(bc_alaw_golden);
	RUN_TEST(bc_g711_sign_symmetry);
	RUN_TEST(bc_dispatch_pcmu_publishes_doubled_pcm);
	RUN_TEST(bc_dispatch_pcma_publishes_doubled_pcm);
	RUN_TEST(bc_dispatch_l16_swaps_network_order);
	RUN_TEST(bc_dispatch_unknown_pt_drops);
	RUN_TEST(bc_dispatch_empty_payload_is_noop);
	RUN_TEST(bc_aac_parse_single_au);
	RUN_TEST(bc_aac_parse_multi_au);
	RUN_TEST(bc_aac_parse_fragment);
	RUN_TEST(bc_aac_parse_malformed);
}

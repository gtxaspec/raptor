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
	rsd_bc_handle(&dec, w, pt, payload, len);
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
}

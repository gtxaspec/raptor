#include "greatest.h"
#include "rmr_prebuf.h"

#include <string.h>

TEST prebuf_push_count(void)
{
	rmr_prebuf_t *pb = rmr_prebuf_create(8, 4096);
	ASSERT(pb);
	uint8_t d[] = {1, 2, 3};
	rmr_prebuf_push(pb, d, sizeof(d), 1000, 0);
	rmr_prebuf_push(pb, d, sizeof(d), 2000, 0);
	rmr_prebuf_push(pb, d, sizeof(d), 3000, 0);
	ASSERT_EQ(3u, rmr_prebuf_count(pb));
	rmr_prebuf_destroy(pb);
	PASS();
}

TEST prebuf_eviction(void)
{
	rmr_prebuf_t *pb = rmr_prebuf_create(4, 4096);
	ASSERT(pb);
	for (int i = 0; i < 6; i++) {
		uint8_t d = (uint8_t)i;
		rmr_prebuf_push(pb, &d, 1, i * 1000, 0);
	}
	/* Capacity is 4, count should not exceed it */
	ASSERT_EQ(4u, rmr_prebuf_count(pb));
	rmr_prebuf_destroy(pb);
	PASS();
}

TEST prebuf_find_keyframe(void)
{
	rmr_prebuf_t *pb = rmr_prebuf_create(8, 4096);
	ASSERT(pb);
	uint8_t d = 0xAA;
	rmr_prebuf_push(pb, &d, 1, 1000, 0); /* P */
	rmr_prebuf_push(pb, &d, 1, 2000, 0); /* P */
	rmr_prebuf_push(pb, &d, 1, 3000, 1); /* I (keyframe) */
	rmr_prebuf_push(pb, &d, 1, 4000, 0); /* P */
	rmr_prebuf_push(pb, &d, 1, 5000, 0); /* P */

	/* Find keyframe with large age limit → should find the I at ts=3000 */
	uint32_t idx = rmr_prebuf_find_keyframe(pb, 10000);
	ASSERT(idx != UINT32_MAX);
	int64_t ts = rmr_prebuf_timestamp(pb, idx);
	ASSERT_EQ(3000, ts);
	rmr_prebuf_destroy(pb);
	PASS();
}

TEST prebuf_find_keyframe_age(void)
{
	rmr_prebuf_t *pb = rmr_prebuf_create(8, 4096);
	ASSERT(pb);
	uint8_t d = 0;
	rmr_prebuf_push(pb, &d, 1, 1000000, 1); /* I, 1s */
	rmr_prebuf_push(pb, &d, 1, 2000000, 0); /* P, 2s */
	rmr_prebuf_push(pb, &d, 1, 3000000, 1); /* I, 3s */
	rmr_prebuf_push(pb, &d, 1, 4000000, 0); /* P, 4s */
	rmr_prebuf_push(pb, &d, 1, 5000000, 0); /* P, 5s */

	/* max_age = 3s: newest is 5s, need keyframe at 2s or older */
	uint32_t idx = rmr_prebuf_find_keyframe(pb, 3000000);
	ASSERT(idx != UINT32_MAX);
	int64_t ts = rmr_prebuf_timestamp(pb, idx);
	/* Should find keyframe at 1s (age 4s >= 3s) */
	ASSERT_EQ(1000000, ts);
	rmr_prebuf_destroy(pb);
	PASS();
}

TEST prebuf_find_frame_at(void)
{
	rmr_prebuf_t *pb = rmr_prebuf_create(8, 4096);
	ASSERT(pb);
	uint8_t d = 0;
	rmr_prebuf_push(pb, &d, 1, 100, 0);
	rmr_prebuf_push(pb, &d, 1, 200, 0);
	rmr_prebuf_push(pb, &d, 1, 300, 0);
	rmr_prebuf_push(pb, &d, 1, 400, 0);

	uint32_t idx = rmr_prebuf_find_frame_at(pb, 250);
	ASSERT(idx != UINT32_MAX);
	int64_t ts = rmr_prebuf_timestamp(pb, idx);
	/* First frame with ts >= 250 is ts=300 */
	ASSERT_EQ(300, ts);
	rmr_prebuf_destroy(pb);
	PASS();
}

struct iter_ctx {
	int count;
	uint8_t data[16];
};

static int iter_cb(const rmr_prebuf_slot_t *slot, const uint8_t *data, void *ctx)
{
	struct iter_ctx *ic = ctx;
	if (ic->count < 16) {
		ic->data[ic->count] = data[0];
		ic->count++;
	}
	(void)slot;
	return 0;
}

TEST prebuf_iterate(void)
{
	rmr_prebuf_t *pb = rmr_prebuf_create(8, 4096);
	ASSERT(pb);
	for (int i = 0; i < 5; i++) {
		uint8_t d = (uint8_t)(0x10 + i);
		rmr_prebuf_push(pb, &d, 1, i * 100, 0);
	}

	uint32_t oldest = rmr_prebuf_write_idx(pb) - rmr_prebuf_count(pb);
	struct iter_ctx ic = {0};
	int n = rmr_prebuf_iterate(pb, oldest, iter_cb, &ic);
	ASSERT_EQ(5, n);
	ASSERT_EQ(5, ic.count);
	for (int i = 0; i < 5; i++)
		ASSERT_EQ((uint8_t)(0x10 + i), ic.data[i]);
	rmr_prebuf_destroy(pb);
	PASS();
}

TEST prebuf_iterate_data(void)
{
	rmr_prebuf_t *pb = rmr_prebuf_create(4, 4096);
	ASSERT(pb);
	/* Push frames with known data, overflow, verify data integrity */
	for (int i = 0; i < 6; i++) {
		uint8_t d = (uint8_t)(0xA0 + i);
		rmr_prebuf_push(pb, &d, 1, i * 100, 0);
	}
	/* Only last 4 frames should be present (0xA2..0xA5) */
	uint32_t oldest = rmr_prebuf_write_idx(pb) - rmr_prebuf_count(pb);
	struct iter_ctx ic = {0};
	rmr_prebuf_iterate(pb, oldest, iter_cb, &ic);
	ASSERT_EQ(4, ic.count);
	ASSERT_EQ(0xA2, ic.data[0]);
	ASSERT_EQ(0xA3, ic.data[1]);
	ASSERT_EQ(0xA4, ic.data[2]);
	ASSERT_EQ(0xA5, ic.data[3]);
	rmr_prebuf_destroy(pb);
	PASS();
}

TEST prebuf_no_keyframe(void)
{
	rmr_prebuf_t *pb = rmr_prebuf_create(4, 4096);
	ASSERT(pb);
	uint8_t d = 0;
	rmr_prebuf_push(pb, &d, 1, 1000, 0);
	rmr_prebuf_push(pb, &d, 1, 2000, 0);
	rmr_prebuf_push(pb, &d, 1, 3000, 0);

	/* No keyframes → find_keyframe returns UINT32_MAX */
	uint32_t idx = rmr_prebuf_find_keyframe(pb, 10000);
	ASSERT_EQ(UINT32_MAX, idx);
	rmr_prebuf_destroy(pb);
	PASS();
}

static int data_iter_cb(const rmr_prebuf_slot_t *slot, const uint8_t *data, void *ctx)
{
	struct iter_ctx *ic = ctx;
	(void)slot;
	/* Verify ALL bytes of the frame, not just the first byte */
	for (uint32_t j = 0; j < slot->data_length; j++) {
		if (data[j] != data[0])
			return -1; /* corruption detected */
	}
	if (ic->count < 16)
		ic->data[ic->count] = data[0];
	ic->count++;
	return 0;
}

TEST prebuf_data_wrap(void)
{
	/* 4 slots, 128-byte data region, 20-byte frames.
	 * Push 8 frames → slots evict oldest 4, data wraps at offset ~120.
	 * The 4 newest frames' data must survive the wrap intact. */
	rmr_prebuf_t *pb = rmr_prebuf_create(4, 128);
	ASSERT(pb);

	/* Push 8 x 20-byte frames.
	 * Data layout: frames 0-5 fill offsets 0,20,40,60,80,100.
	 * Frame 6: 120+20=140>128 → wraps to offset 0. Frame 7: offset 20.
	 * Slots keep frames 4-7. Their data:
	 *   Frame 4: offset 80, intact (not overwritten)
	 *   Frame 5: offset 100, intact
	 *   Frame 6: offset 0 (wrapped), intact
	 *   Frame 7: offset 20, intact */
	for (int i = 0; i < 8; i++) {
		uint8_t frame[20];
		memset(frame, 0x30 + i, sizeof(frame));
		rmr_prebuf_push(pb, frame, sizeof(frame), i * 1000, 0);
	}

	ASSERT_EQ(4u, rmr_prebuf_count(pb));

	/* Iterate the 4 valid frames, verify data integrity across the wrap */
	uint32_t oldest = rmr_prebuf_write_idx(pb) - rmr_prebuf_count(pb);
	struct iter_ctx ic = {0};
	int n = rmr_prebuf_iterate(pb, oldest, data_iter_cb, &ic);
	ASSERT_EQ(4, n);
	ASSERT_EQ(4, ic.count);

	/* Frames 4-7: values 0x34..0x37 */
	for (int i = 0; i < 4; i++)
		ASSERT_EQ((uint8_t)(0x34 + i), ic.data[i]);

	rmr_prebuf_destroy(pb);
	PASS();
}

SUITE(prebuf_suite)
{
	RUN_TEST(prebuf_push_count);
	RUN_TEST(prebuf_eviction);
	RUN_TEST(prebuf_find_keyframe);
	RUN_TEST(prebuf_find_keyframe_age);
	RUN_TEST(prebuf_find_frame_at);
	RUN_TEST(prebuf_iterate);
	RUN_TEST(prebuf_iterate_data);
	RUN_TEST(prebuf_no_keyframe);
	RUN_TEST(prebuf_data_wrap);
}

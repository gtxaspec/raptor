/*
 * Unit tests for ring buffer (rss_ring) — sequence tracking, overflow
 * recovery, IDR request, acquire/release, stream info.
 *
 * Ring API contract:
 *   - write_seq starts at 0, first publish increments to 1
 *   - read_seq should be initialized to write_seq to read next frame
 *   - read returns -EAGAIN when read_seq >= write_seq (no new data)
 *   - read returns RSS_EOVERFLOW when consumer fell behind by >= slot_count
 */

#include "greatest.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <rss_ipc.h>
#include <rss_common.h>

/* ── Helpers ── */

static rss_ring_t *make_ring(const char *name, uint32_t slots, uint32_t data_size)
{
	return rss_ring_create(name, slots, data_size);
}

static void publish_frame(rss_ring_t *r, const uint8_t *data, uint32_t len, int64_t ts,
			  uint16_t nal_type, bool is_key)
{
	rss_iov_t iov = {.data = data, .length = len};
	rss_ring_publish_iov(r, &iov, 1, ts, nal_type, is_key ? 1 : 0);
}

/*
 * Ring sequence convention:
 *   - write_seq starts at 0, first publish sets it to 1
 *   - Frame N is stored in slot[N % slot_count] with slot.seq = N
 *   - Consumer can read frames with seq in [1, write_seq-1]
 *   - read_seq = write_seq is NOT readable (latest is "in flight")
 *   - read_seq = 0 reads uninitialized slot[0] — skip it
 *   - Practical: consumer starts at seq=1, reads until seq >= write_seq
 *
 * For tests: publish 2+ frames, read starting at seq=1.
 */

/* ── Tests ── */

TEST ring_create_destroy(void)
{
	rss_ring_t *r = rss_ring_create("test_cd", 4, 4096);
	ASSERT(r);
	const rss_ring_header_t *hdr = rss_ring_get_header(r);
	ASSERT(hdr);
	ASSERT_EQ(4, (int)hdr->slot_count);
	ASSERT_EQ(0, (int)hdr->write_seq);
	rss_ring_destroy(r);
	PASS();
}

TEST ring_open_close(void)
{
	rss_ring_t *w = rss_ring_create("test_oc", 4, 4096);
	ASSERT(w);

	rss_ring_t *rd = rss_ring_open("test_oc");
	ASSERT(rd);

	rss_ring_close(rd);
	rss_ring_destroy(w);
	PASS();
}

TEST ring_publish_read_basic(void)
{
	rss_ring_t *w = make_ring("test_prb", 4, 4096);
	ASSERT(w);
	rss_ring_t *rd = rss_ring_open("test_prb");
	ASSERT(rd);

	uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
	publish_frame(w, data, sizeof(data), 1000, 0x13, true);
	/* Second publish makes first readable (write_seq advances past it) */
	uint8_t dummy[] = {0};
	publish_frame(w, dummy, sizeof(dummy), 2000, 0x14, false);

	uint64_t seq = 1; /* first real frame */
	uint8_t buf[256];
	uint32_t len;
	rss_ring_slot_t meta;
	int ret = rss_ring_read(rd, &seq, buf, sizeof(buf), &len, &meta);
	ASSERT_EQ(0, ret);
	ASSERT_EQ(4, (int)len);
	ASSERT_MEM_EQ(data, buf, 4);
	ASSERT_EQ(1000, (int)meta.timestamp);
	ASSERT_EQ(1, (int)meta.is_key);

	rss_ring_close(rd);
	rss_ring_destroy(w);
	PASS();
}

TEST ring_sequence_tracking(void)
{
	rss_ring_t *w = make_ring("test_seq", 8, 8192);
	ASSERT(w);
	rss_ring_t *rd = rss_ring_open("test_seq");
	ASSERT(rd);

	uint8_t data[64];
	memset(data, 0xAA, sizeof(data));

	/* Publish 6 frames — first 5 readable, 6th makes 5th readable */
	for (int i = 0; i < 6; i++)
		publish_frame(w, data, sizeof(data), i * 40000, 0x14, i == 0);

	/* Read 5 frames starting at seq=1 */
	uint64_t seq = 1;
	for (int i = 0; i < 5; i++) {
		uint8_t buf[256];
		uint32_t len;
		rss_ring_slot_t meta;
		int ret = rss_ring_read(rd, &seq, buf, sizeof(buf), &len, &meta);
		ASSERT_EQ(0, ret);
		ASSERT_EQ(i * 40000, (int)meta.timestamp);
	}

	/* seq=6 should have the last frame (ts=5*40000), but write_seq=6
	 * so read at 6 → EAGAIN. One unreadable "in flight" frame. */

	rss_ring_close(rd);
	rss_ring_destroy(w);
	PASS();
}

TEST ring_overflow_detection(void)
{
	/* Small ring: 4 slots */
	rss_ring_t *w = make_ring("test_ovf", 4, 4096);
	ASSERT(w);
	rss_ring_t *rd = rss_ring_open("test_ovf");
	ASSERT(rd);

	uint8_t data[64];
	memset(data, 0xBB, sizeof(data));

	/* Publish 2 frames, read first normally */
	publish_frame(w, data, sizeof(data), 0, 0x13, true);
	publish_frame(w, data, sizeof(data), 40000, 0x14, false);
	uint64_t seq = 1;
	uint8_t buf[256];
	uint32_t len;
	rss_ring_slot_t meta;
	int ret = rss_ring_read(rd, &seq, buf, sizeof(buf), &len, &meta);
	ASSERT_EQ(0, ret);

	/* Now publish 8 more without reading — overflows 4-slot ring */
	for (int i = 0; i < 8; i++)
		publish_frame(w, data, sizeof(data), (i + 2) * 40000, 0x14, false);

	/* Read should return RSS_EOVERFLOW */
	ret = rss_ring_read(rd, &seq, buf, sizeof(buf), &len, &meta);
	ASSERT_EQ(RSS_EOVERFLOW, ret);

	rss_ring_close(rd);
	rss_ring_destroy(w);
	PASS();
}

TEST ring_overflow_recovery(void)
{
	rss_ring_t *w = make_ring("test_ovr", 4, 4096);
	ASSERT(w);
	rss_ring_t *rd = rss_ring_open("test_ovr");
	ASSERT(rd);

	uint8_t data[64];
	memset(data, 0xCC, sizeof(data));

	/* Publish 2 frames, read first normally */
	publish_frame(w, data, sizeof(data), 0, 0x13, true);
	publish_frame(w, data, sizeof(data), 40000, 0x14, false);
	uint64_t seq = 1;
	uint8_t buf[256];
	uint32_t len;
	rss_ring_slot_t meta;
	rss_ring_read(rd, &seq, buf, sizeof(buf), &len, &meta);

	/* Overflow: publish 8 more without reading */
	for (int i = 0; i < 8; i++)
		publish_frame(w, data, sizeof(data), (i + 2) * 40000, 0x14, false);

	/* Should get overflow */
	int ret = rss_ring_read(rd, &seq, buf, sizeof(buf), &len, &meta);
	ASSERT_EQ(RSS_EOVERFLOW, ret);

	/* Recovery: seq was advanced by the overflow, read latest */
	const rss_ring_header_t *hdr = rss_ring_get_header(rd);
	if (seq > hdr->write_seq)
		seq = hdr->write_seq > 0 ? hdr->write_seq - 1 : 0;
	else
		seq = hdr->write_seq > 0 ? hdr->write_seq - 1 : 0;

	ret = rss_ring_read(rd, &seq, buf, sizeof(buf), &len, &meta);
	ASSERT_EQ(0, ret);

	rss_ring_close(rd);
	rss_ring_destroy(w);
	PASS();
}

TEST ring_idr_request(void)
{
	rss_ring_t *w = make_ring("test_idr", 4, 4096);
	ASSERT(w);
	rss_ring_t *rd = rss_ring_open("test_idr");
	ASSERT(rd);

	const rss_ring_header_t *hdr = rss_ring_get_header(w);
	ASSERT_EQ(0, (int)hdr->idr_request);

	/* Reader requests IDR */
	rss_ring_request_idr(rd);
	ASSERT_EQ(1, (int)hdr->idr_request);

	rss_ring_close(rd);
	rss_ring_destroy(w);
	PASS();
}

TEST ring_stream_info(void)
{
	rss_ring_t *w = make_ring("test_si", 4, 4096);
	ASSERT(w);
	rss_ring_set_stream_info(w, 0, 0, 1920, 1080, 25, 1, 100, 40);

	const rss_ring_header_t *hdr = rss_ring_get_header(w);
	ASSERT_EQ(0, (int)hdr->codec);
	ASSERT_EQ(1920, (int)hdr->width);
	ASSERT_EQ(1080, (int)hdr->height);
	ASSERT_EQ(25, (int)hdr->fps_num);
	ASSERT_EQ(1, (int)hdr->fps_den);

	rss_ring_destroy(w);
	PASS();
}

TEST ring_acquire_release(void)
{
	rss_ring_t *w = make_ring("test_ar", 4, 4096);
	ASSERT(w);
	rss_ring_t *r1 = rss_ring_open("test_ar");
	rss_ring_t *r2 = rss_ring_open("test_ar");
	ASSERT(r1);
	ASSERT(r2);

	ASSERT_EQ(0, (int)rss_ring_reader_count(w));

	rss_ring_acquire(r1);
	ASSERT_EQ(1, (int)rss_ring_reader_count(w));

	rss_ring_acquire(r2);
	ASSERT_EQ(2, (int)rss_ring_reader_count(w));

	rss_ring_release(r1);
	ASSERT_EQ(1, (int)rss_ring_reader_count(w));

	rss_ring_release(r2);
	ASSERT_EQ(0, (int)rss_ring_reader_count(w));

	rss_ring_close(r1);
	rss_ring_close(r2);
	rss_ring_destroy(w);
	PASS();
}

TEST ring_large_frame(void)
{
	rss_ring_t *w = make_ring("test_lf", 4, 256 * 1024);
	ASSERT(w);
	rss_ring_t *rd = rss_ring_open("test_lf");
	ASSERT(rd);

	uint8_t *big = calloc(1, 32768);
	ASSERT(big);
	memset(big, 0xDD, 32768);
	big[0] = 0x42;
	big[32767] = 0x43;

	publish_frame(w, big, 32768, 100000, 0x13, true);
	/* Publish dummy to make first frame readable */
	uint8_t dummy[] = {0};
	publish_frame(w, dummy, sizeof(dummy), 200000, 0x14, false);
	uint64_t seq = 1;

	uint8_t *rbuf = calloc(1, 65536);
	ASSERT(rbuf);
	uint32_t len;
	rss_ring_slot_t meta;
	int ret = rss_ring_read(rd, &seq, rbuf, 65536, &len, &meta);
	ASSERT_EQ(0, ret);
	ASSERT_EQ(32768, (int)len);
	ASSERT_EQ(0x42, rbuf[0]);
	ASSERT_EQ(0x43, rbuf[32767]);

	free(big);
	free(rbuf);
	rss_ring_close(rd);
	rss_ring_destroy(w);
	PASS();
}

TEST ring_multiple_readers(void)
{
	rss_ring_t *w = make_ring("test_mr", 8, 4096);
	ASSERT(w);
	rss_ring_t *r1 = rss_ring_open("test_mr");
	rss_ring_t *r2 = rss_ring_open("test_mr");
	ASSERT(r1);
	ASSERT(r2);

	uint8_t data[] = {0x01, 0x02, 0x03};
	publish_frame(w, data, sizeof(data), 5000, 0x14, false);
	uint8_t dummy[] = {0};
	publish_frame(w, dummy, sizeof(dummy), 6000, 0x14, false);

	uint64_t seq1 = 1;
	uint64_t seq2 = 1;

	/* Both readers should get the same frame */
	uint8_t buf1[64], buf2[64];
	uint32_t len1, len2;
	rss_ring_slot_t m1, m2;

	ASSERT_EQ(0, rss_ring_read(r1, &seq1, buf1, sizeof(buf1), &len1, &m1));
	ASSERT_EQ(0, rss_ring_read(r2, &seq2, buf2, sizeof(buf2), &len2, &m2));

	ASSERT_EQ((int)len1, (int)len2);
	ASSERT_MEM_EQ(buf1, buf2, len1);
	ASSERT_EQ((int)seq1, (int)seq2);

	rss_ring_close(r1);
	rss_ring_close(r2);
	rss_ring_destroy(w);
	PASS();
}

TEST ring_dest_too_small(void)
{
	rss_ring_t *w = make_ring("test_dts", 4, 4096);
	ASSERT(w);
	rss_ring_t *rd = rss_ring_open("test_dts");
	ASSERT(rd);

	uint8_t data[256];
	memset(data, 0xEE, sizeof(data));
	publish_frame(w, data, sizeof(data), 1000, 0x14, false);
	uint8_t dummy[] = {0};
	publish_frame(w, dummy, sizeof(dummy), 2000, 0x14, false);
	uint64_t seq = 1;

	/* Try to read into a buffer that's too small — should return -ENOSPC */
	uint8_t buf[16];
	uint32_t len;
	rss_ring_slot_t meta;
	int ret = rss_ring_read(rd, &seq, buf, sizeof(buf), &len, &meta);
	ASSERT_EQ(-ENOSPC, ret);
	ASSERT_EQ(256, (int)len); /* reports actual frame size */

	rss_ring_close(rd);
	rss_ring_destroy(w);
	PASS();
}

TEST ring_no_data_available(void)
{
	rss_ring_t *w = make_ring("test_nda", 4, 4096);
	ASSERT(w);
	rss_ring_t *rd = rss_ring_open("test_nda");
	ASSERT(rd);

	uint64_t seq = 1;

	/* Read from empty ring */
	uint8_t buf[64];
	uint32_t len;
	rss_ring_slot_t meta;
	int ret = rss_ring_read(rd, &seq, buf, sizeof(buf), &len, &meta);
	ASSERT_EQ(-EAGAIN, ret);

	rss_ring_close(rd);
	rss_ring_destroy(w);
	PASS();
}

TEST ring_keyframe_flag(void)
{
	rss_ring_t *w = make_ring("test_kf", 8, 4096);
	ASSERT(w);
	rss_ring_t *rd = rss_ring_open("test_kf");
	ASSERT(rd);

	uint8_t data[32];
	memset(data, 0, sizeof(data));

	/* Publish IDR (key=1), P-frame (key=0), dummy (makes P readable) */
	publish_frame(w, data, sizeof(data), 0, 0x13, true);
	publish_frame(w, data, sizeof(data), 40000, 0x14, false);
	publish_frame(w, data, sizeof(data), 80000, 0x14, false);

	uint64_t seq = 1;

	uint8_t buf[64];
	uint32_t len;
	rss_ring_slot_t meta;

	rss_ring_read(rd, &seq, buf, sizeof(buf), &len, &meta);
	ASSERT_EQ(1, (int)meta.is_key);
	ASSERT_EQ(0x13, (int)meta.nal_type);

	rss_ring_read(rd, &seq, buf, sizeof(buf), &len, &meta);
	ASSERT_EQ(0, (int)meta.is_key);
	ASSERT_EQ(0x14, (int)meta.nal_type);

	rss_ring_close(rd);
	rss_ring_destroy(w);
	PASS();
}

SUITE(ring_suite)
{
	RUN_TEST(ring_create_destroy);
	RUN_TEST(ring_open_close);
	RUN_TEST(ring_publish_read_basic);
	RUN_TEST(ring_sequence_tracking);
	RUN_TEST(ring_overflow_detection);
	RUN_TEST(ring_overflow_recovery);
	RUN_TEST(ring_idr_request);
	RUN_TEST(ring_stream_info);
	RUN_TEST(ring_acquire_release);
	RUN_TEST(ring_large_frame);
	RUN_TEST(ring_multiple_readers);
	RUN_TEST(ring_dest_too_small);
	RUN_TEST(ring_no_data_available);
	RUN_TEST(ring_keyframe_flag);
}

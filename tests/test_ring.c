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
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/mman.h>
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

	/* Publish 6 frames — every one readable, including the newest */
	for (int i = 0; i < 6; i++)
		publish_frame(w, data, sizeof(data), i * 40000, 0x14, i == 0);

	uint64_t seq = 1;
	for (int i = 0; i < 6; i++) {
		uint8_t buf[256];
		uint32_t len;
		rss_ring_slot_t meta;
		int ret = rss_ring_read(rd, &seq, buf, sizeof(buf), &len, &meta);
		ASSERT_EQ(0, ret);
		ASSERT_EQ(i * 40000, (int)meta.timestamp);
	}

	/* Fully drained: the next read has nothing to return */
	{
		uint8_t buf[256];
		uint32_t len;
		rss_ring_slot_t meta;
		ASSERT_EQ(-EAGAIN, rss_ring_read(rd, &seq, buf, sizeof(buf), &len, &meta));
	}

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

/* The stream-info cluster is rewritten in place across an encoder restart
 * while consumers poll it live; the seqlock snapshot must never hand back
 * a hybrid of two publishes. Hammer two distinct tuples from a writer
 * thread and require every successful snapshot to be exactly one of them
 * in ALL eight fields. */
struct si_hammer {
	rss_ring_t *ring;
	_Atomic bool stop;
};

static void *si_hammer_writer(void *arg)
{
	struct si_hammer *hm = arg;
	for (uint32_t i = 0; !atomic_load(&hm->stop); i++) {
		if (i & 1)
			rss_ring_set_stream_info(hm->ring, 0, 1, 1280, 720, 25, 1, 77, 31);
		else
			rss_ring_set_stream_info(hm->ring, 0, 0, 1920, 1080, 30, 1, 100, 40);
	}
	return NULL;
}

TEST ring_stream_info_never_tears(void)
{
	rss_ring_t *w = make_ring("test_si_tear", 4, 4096);
	ASSERT(w);
	rss_ring_set_stream_info(w, 0, 0, 1920, 1080, 30, 1, 100, 40);
	rss_ring_t *r = rss_ring_open("test_si_tear");
	ASSERT(r);

	struct si_hammer hm = {.ring = w};
	atomic_init(&hm.stop, false);
	pthread_t wr;
	ASSERT_EQ(0, pthread_create(&wr, NULL, si_hammer_writer, &hm));

	int ok = 0, again = 0;
	for (int i = 0; i < 200000; i++) {
		rss_stream_info_t si;
		if (rss_ring_get_stream_info(r, &si) != 0) {
			again++;
			continue;
		}
		ok++;
		bool a = (si.codec == 0 && si.width == 1920 && si.height == 1080 &&
			  si.fps_num == 30 && si.fps_den == 1 && si.profile == 100 &&
			  si.level == 40);
		bool b =
			(si.codec == 1 && si.width == 1280 && si.height == 720 &&
			 si.fps_num == 25 && si.fps_den == 1 && si.profile == 77 && si.level == 31);
		if (!a && !b) {
			atomic_store(&hm.stop, true);
			pthread_join(wr, NULL);
			FAILm("torn stream-info snapshot observed");
		}
	}
	atomic_store(&hm.stop, true);
	pthread_join(wr, NULL);

	/* The writer never rests, so some -EAGAIN is expected; a reader that
	 * never succeeds means the retry bound is wrong. */
	ASSERT(ok > 0);
	(void)again;

	rss_ring_close(r);
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

	/* Publish IDR (key=1) then P-frame (key=0) */
	publish_frame(w, data, sizeof(data), 0, 0x13, true);
	publish_frame(w, data, sizeof(data), 40000, 0x14, false);

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

TEST ring_newest_frame_readable(void)
{
	/* The regression that lost the last frame of every finite stream:
	 * with 1-based sequences, seq == write_seq is a fully published
	 * frame and must be readable without waiting for a successor. */
	rss_ring_t *w = make_ring("test_nfr", 4, 4096);
	ASSERT(w);
	rss_ring_t *rd = rss_ring_open("test_nfr");
	ASSERT(rd);

	uint8_t data[] = {0x5A, 0xA5};
	publish_frame(w, data, sizeof(data), 7000, 0x14, false);

	uint64_t seq = 1;
	uint8_t buf[64];
	uint32_t len;
	rss_ring_slot_t meta;
	ASSERT_EQ(0, rss_ring_read(rd, &seq, buf, sizeof(buf), &len, &meta));
	ASSERT_EQ(2, (int)len);
	ASSERT_MEM_EQ(data, buf, 2);
	ASSERT_EQ(2, (int)seq);

	rss_ring_close(rd);
	rss_ring_destroy(w);
	PASS();
}

TEST ring_peek_newest_frame(void)
{
	rss_ring_t *w = make_ring("test_pnf", 4, 4096);
	ASSERT(w);
	rss_ring_t *rd = rss_ring_open("test_pnf");
	ASSERT(rd);

	uint8_t data[] = {0x11, 0x22, 0x33};
	publish_frame(w, data, sizeof(data), 9000, 0x14, false);

	uint64_t seq = 1;
	const uint8_t *ptr = NULL;
	uint32_t len;
	rss_ring_slot_t meta;
	ASSERT_EQ(0, rss_ring_peek(rd, &seq, &ptr, &len, &meta));
	ASSERT(ptr);
	ASSERT_EQ(3, (int)len);
	ASSERT_MEM_EQ(data, ptr, 3);
	ASSERT_EQ(0, rss_ring_peek_done(rd, &meta));

	rss_ring_close(rd);
	rss_ring_destroy(w);
	PASS();
}

TEST ring_cold_start_seq_zero(void)
{
	/* read_seq = 0 is the common cold-start init. Empty ring: EAGAIN,
	 * never a resync loop. Non-empty: one overflow resyncs to the
	 * newest frame, which is then readable. */
	rss_ring_t *w = make_ring("test_cs0", 4, 4096);
	ASSERT(w);
	rss_ring_t *rd = rss_ring_open("test_cs0");
	ASSERT(rd);

	uint64_t seq = 0;
	uint8_t buf[64];
	uint32_t len;
	rss_ring_slot_t meta;
	ASSERT_EQ(-EAGAIN, rss_ring_read(rd, &seq, buf, sizeof(buf), &len, &meta));
	ASSERT_EQ(0, (int)seq);

	uint8_t data[] = {0x77};
	publish_frame(w, data, sizeof(data), 100, 0x14, false);
	publish_frame(w, data, sizeof(data), 200, 0x14, false);

	ASSERT_EQ(RSS_EOVERFLOW, rss_ring_read(rd, &seq, buf, sizeof(buf), &len, &meta));
	ASSERT_EQ(2, (int)seq);
	ASSERT_EQ(0, rss_ring_read(rd, &seq, buf, sizeof(buf), &len, &meta));
	ASSERT_EQ(200, (int)meta.timestamp);

	rss_ring_close(rd);
	rss_ring_destroy(w);
	PASS();
}

TEST ring_stale_detection(void)
{
	rss_ring_t *w = make_ring("test_stale", 4, 4096);
	ASSERT(w);
	rss_ring_t *rd = rss_ring_open("test_stale");
	ASSERT(rd);

	/* Live producer: not stale. */
	ASSERT_EQ(false, rss_ring_stale(rd));

	/* Producer dies and is reborn: same name, new file. The old
	 * handle's mapping is frozen and must report stale. */
	rss_ring_destroy(w);
	ASSERT_EQ(true, rss_ring_stale(rd));

	w = make_ring("test_stale", 4, 4096);
	ASSERT(w);
	ASSERT_EQ(true, rss_ring_stale(rd));

	/* A fresh open resolves to the reborn file: not stale. */
	rss_ring_t *rd2 = rss_ring_open("test_stale");
	ASSERT(rd2);
	ASSERT_EQ(false, rss_ring_stale(rd2));

	rss_ring_close(rd);
	rss_ring_close(rd2);
	rss_ring_destroy(w);
	PASS();
}

TEST ring_producer_survives_recreate_larger(void)
{
	/* A ring re-created larger while an old producer still holds it:
	 * the old handle's mapping is the ORIGINAL size, but data_size in
	 * the shared header now describes the new, bigger region. A
	 * producer that bounds its writes by the header walks off the end
	 * of its own mapping -- CI caught exactly that as a SEGV inside
	 * rss_ring_publish_iov's memcpy during the leak soak's reconnect
	 * churn. Publishing from the stale handle must fail, not write. */
	rss_ring_t *old = make_ring("test_recr", 4, 4096);
	ASSERT(old);

	rss_ring_t *fresh = rss_ring_create("test_recr", 8, 262144);
	ASSERT(fresh);

	uint8_t payload[65536];
	memset(payload, 0x5A, sizeof(payload));
	/* Larger than the old mapping's whole data region, smaller than
	 * the new one's: the size the header now advertises. */
	int ret = rss_ring_publish(old, payload, sizeof(payload), 1000, 0x14, false);
	ASSERT(ret != 0);

	rss_ring_destroy(fresh);
	rss_ring_destroy(old);
	PASS();
}

TEST ring_refmode_producer_superseded(void)
{
	/* The refmode twin of the test above, and the path devices
	 * actually run: frame data lives in external shm, but the slot
	 * array is still inside this handle's mapping, so a re-create with
	 * more slots would index past its end. Publishing must be refused.
	 * No /dev/rmem needed -- refmode resolves named POSIX shm first. */
	rss_ring_t *old = make_ring("test_refsup", 4, 4096);
	ASSERT(old);
	ASSERT_EQ(0, rss_ring_enable_refmode(old, 65536, 0, 2, 32768));

	/* Publishing works while this handle owns the ring. */
	ASSERT_EQ(0, rss_ring_publish_ref(old, 0, 1024, 5000, 0x14, false, 0));

	/* Someone re-creates it with more slots; our slot array is still
	 * the old, smaller one. */
	rss_ring_t *fresh = rss_ring_create("test_refsup", 32, 4096);
	ASSERT(fresh);
	ASSERT_EQ(0, rss_ring_enable_refmode(fresh, 65536, 0, 2, 32768));

	ASSERT_EQ(-EPIPE, rss_ring_publish_ref(old, 0, 1024, 6000, 0x14, false, 0));

	rss_ring_destroy(fresh);
	rss_ring_destroy(old);
	PASS();
}

/* Fill a named enc SHM ("/rss_enc_<ring>") with a byte pattern, the way
 * the producer's SHM injection does. Returns the mapped size or 0. */
static uint32_t make_enc_shm(const char *ring_name, uint32_t size, uint8_t pattern)
{
	char name[128];
	snprintf(name, sizeof(name), "/rss_enc_%s", ring_name);
	shm_unlink(name);
	int fd = shm_open(name, O_CREAT | O_RDWR | O_EXCL, 0600);
	if (fd < 0)
		return 0;
	if (ftruncate(fd, size) != 0) {
		close(fd);
		return 0;
	}
	uint8_t *p = mmap(NULL, size, PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (p == MAP_FAILED)
		return 0;
	memset(p, pattern, size);
	munmap(p, size);
	return size;
}

/* An encoder restart on a REUSED ring replaces the backing region: the
 * producer re-injects a fresh enc SHM and re-enables refmode. A consumer
 * holding the mapping from open must follow (ref_gen bump -> remap) or it
 * keeps serving the old object's frozen bytes under fresh slot metadata --
 * the exact corruption a live set-resolution produced on the wire. */
TEST ring_refmode_remap_after_region_swap(void)
{
	rss_ring_t *w = make_ring("test_refswap", 8, 4096);
	ASSERT(w);
	ASSERT(make_enc_shm("test_refswap", 65536, 0xAA));
	ASSERT_EQ(0, rss_ring_enable_refmode(w, 65536, 0, 2, 32768));
	ASSERT_EQ(0, rss_ring_publish_ref(w, 0, 64, 1000, 0x14, true, 0));

	rss_ring_t *rd = rss_ring_open("test_refswap");
	ASSERT(rd);

	uint8_t buf[256];
	uint32_t len;
	rss_ring_slot_t meta;
	uint64_t seq = 1;
	ASSERT_EQ(0, rss_ring_read(rd, &seq, buf, sizeof(buf), &len, &meta));
	ASSERT_EQ(64, (int)len);
	ASSERT_EQ(0xAA, buf[0]);
	ASSERT_EQ(0xAA, buf[63]);

	/* Encoder restart: new object under the same name, new contents,
	 * re-enabled refmode. The old mapping is now a lie. */
	ASSERT(make_enc_shm("test_refswap", 131072, 0xBB));
	ASSERT_EQ(0, rss_ring_enable_refmode(w, 131072, 0, 2, 65536));
	ASSERT_EQ(0, rss_ring_publish_ref(w, 0, 64, 2000, 0x14, true, 0));

	ASSERT_EQ(0, rss_ring_read(rd, &seq, buf, sizeof(buf), &len, &meta));
	ASSERT_EQ(64, (int)len);
	ASSERT_EQ_FMT(0xBB, buf[0], "0x%02x");
	ASSERT_EQ_FMT(0xBB, buf[63], "0x%02x");

	rss_ring_close(rd);
	rss_ring_destroy(w);
	shm_unlink("/rss_enc_test_refswap");
	PASS();
}

TEST ring_open_handle_cannot_publish(void)
{
	/* rss_ring_open() yields a consumer handle, and every publish path
	 * rejects those. Code that opens-then-publishes therefore throws
	 * its data away silently unless it checks the return -- which is
	 * what rsd's backchannel did whenever the speaker ring already
	 * existed. */
	rss_ring_t *owner = make_ring("test_ownpub", 4, 4096);
	ASSERT(owner);

	rss_ring_t *opened = rss_ring_open("test_ownpub");
	ASSERT(opened);

	uint8_t data[] = {0x01, 0x02};
	ASSERT_EQ(-EINVAL, rss_ring_publish(opened, data, sizeof(data), 1000, 0x14, false));
	/* A create on the same name does yield a producer handle. */
	rss_ring_t *taken = rss_ring_create("test_ownpub", 4, 4096);
	ASSERT(taken);
	ASSERT_EQ(0, rss_ring_publish(taken, data, sizeof(data), 2000, 0x14, false));

	rss_ring_close(opened);
	rss_ring_destroy(taken);
	rss_ring_destroy(owner);
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
	RUN_TEST(ring_stream_info_never_tears);
	RUN_TEST(ring_acquire_release);
	RUN_TEST(ring_large_frame);
	RUN_TEST(ring_multiple_readers);
	RUN_TEST(ring_dest_too_small);
	RUN_TEST(ring_no_data_available);
	RUN_TEST(ring_keyframe_flag);
	RUN_TEST(ring_newest_frame_readable);
	RUN_TEST(ring_peek_newest_frame);
	RUN_TEST(ring_cold_start_seq_zero);
	RUN_TEST(ring_stale_detection);
	RUN_TEST(ring_producer_survives_recreate_larger);
	RUN_TEST(ring_refmode_producer_superseded);
	RUN_TEST(ring_refmode_remap_after_region_swap);
	RUN_TEST(ring_open_handle_cannot_publish);
}

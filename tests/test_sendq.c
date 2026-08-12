/*
 * test_sendq.c -- Unit tests for rsd's per-client send queue
 *
 * The drop policies here are load-bearing for A/V behavior on slow
 * clients and shipped untested until a field report (PR #27) showed
 * the video overflow path erasing queued audio. Pins: video overflow
 * discards only video and never enqueues the triggering frame; queued
 * audio survives in arrival order with timestamps intact; the audio
 * overflow path evicts exactly one oldest audio entry and never
 * touches video; an all-video queue drops the incoming audio rather
 * than evicting video behind the send thread's back.
 */
#include <string.h>

#include "greatest.h"
#include "../rsd/rsd_sendq.h"

static uint8_t payload[16] = { 0xaa, 0xbb, 0xcc, 0xdd };

static int push_a(rsd_sendq_t *q, uint32_t ts)
{
	return rsd_sendq_push_audio(q, 8, payload, sizeof(payload), ts, (int64_t)ts * 100);
}

static int push_v(rsd_sendq_t *q, uint32_t ts)
{
	return rsd_sendq_push_video(q, payload, sizeof(payload), ts, NULL, 0, false);
}

/* Walk the queue oldest-first, collecting (type, ts) into arrays. */
static int snapshot(rsd_sendq_t *q, uint8_t *types, uint32_t *ts, int max)
{
	int n = 0;
	for (int i = 0; i < q->count && n < max; i++) {
		int idx = (q->tail + i) % RSD_SENDQ_SLOTS;
		types[n] = q->entries[idx].type;
		ts[n] = q->entries[idx].rtp_ts;
		n++;
	}
	return n;
}

TEST push_pop_order_preserved(void)
{
	rsd_sendq_t q;
	ASSERT_EQ(0, rsd_sendq_init(&q));

	ASSERT_EQ(RSD_SENDQ_OK, push_v(&q, 100));
	ASSERT_EQ(RSD_SENDQ_OK, push_a(&q, 101));
	ASSERT_EQ(RSD_SENDQ_OK, push_v(&q, 102));
	ASSERT_EQ(RSD_SENDQ_OK, push_a(&q, 103));

	uint8_t types[8];
	uint32_t ts[8];
	ASSERT_EQ(4, snapshot(&q, types, ts, 8));
	ASSERT_EQ(RSD_FRAME_VIDEO, types[0]);
	ASSERT_EQ(100u, ts[0]);
	ASSERT_EQ(RSD_FRAME_AUDIO, types[1]);
	ASSERT_EQ(101u, ts[1]);
	ASSERT_EQ(RSD_FRAME_VIDEO, types[2]);
	ASSERT_EQ(RSD_FRAME_AUDIO, types[3]);

	rsd_sendq_destroy(&q);
	PASS();
}

TEST video_overflow_keeps_ordered_audio(void)
{
	rsd_sendq_t q;
	ASSERT_EQ(0, rsd_sendq_init(&q));

	/* Fill completely: audio every 4th entry, distinct timestamps. */
	int audio_in = 0;
	for (int i = 0; i < RSD_SENDQ_SLOTS; i++) {
		if (i % 4 == 3) {
			ASSERT_EQ(RSD_SENDQ_OK, push_a(&q, 1000u + (uint32_t)i));
			audio_in++;
		} else {
			ASSERT_EQ(RSD_SENDQ_OK, push_v(&q, 1000u + (uint32_t)i));
		}
	}
	ASSERT_EQ(RSD_SENDQ_SLOTS, q.count);

	/* The overflowing video push: dropped, not enqueued. */
	ASSERT_EQ(RSD_SENDQ_DROPPED, push_v(&q, 9999));

	ASSERT_EQ(audio_in, q.count);
	ASSERT_EQ(1u, q.overflows);
	/* Every queued video plus the incoming frame is accounted. */
	ASSERT_EQ((uint32_t)(RSD_SENDQ_SLOTS - audio_in) + 1u, q.drop_video);
	ASSERT_EQ(0u, q.drop_audio);

	uint8_t types[RSD_SENDQ_SLOTS];
	uint32_t ts[RSD_SENDQ_SLOTS];
	int n = snapshot(&q, types, ts, RSD_SENDQ_SLOTS);
	ASSERT_EQ(audio_in, n);
	uint32_t prev = 0;
	for (int i = 0; i < n; i++) {
		ASSERT_EQ(RSD_FRAME_AUDIO, types[i]);
		ASSERT(ts[i] > prev); /* arrival order, timestamps intact */
		prev = ts[i];
	}

	/* The queue keeps working afterwards. */
	ASSERT_EQ(RSD_SENDQ_OK, push_v(&q, 10000));
	ASSERT_EQ(audio_in + 1, q.count);

	rsd_sendq_destroy(&q);
	PASS();
}

TEST video_overflow_all_video_empties(void)
{
	rsd_sendq_t q;
	ASSERT_EQ(0, rsd_sendq_init(&q));

	for (int i = 0; i < RSD_SENDQ_SLOTS; i++)
		ASSERT_EQ(RSD_SENDQ_OK, push_v(&q, (uint32_t)i));
	ASSERT_EQ(RSD_SENDQ_DROPPED, push_v(&q, 999));

	ASSERT_EQ(0, q.count);
	ASSERT_EQ((uint32_t)RSD_SENDQ_SLOTS + 1u, q.drop_video);

	rsd_sendq_destroy(&q);
	PASS();
}

TEST audio_overflow_evicts_one_oldest_audio(void)
{
	rsd_sendq_t q;
	ASSERT_EQ(0, rsd_sendq_init(&q));

	/* Two audio entries near the front, the rest video. */
	ASSERT_EQ(RSD_SENDQ_OK, push_v(&q, 1));
	ASSERT_EQ(RSD_SENDQ_OK, push_a(&q, 2));
	ASSERT_EQ(RSD_SENDQ_OK, push_a(&q, 3));
	for (int i = 0; i < RSD_SENDQ_SLOTS - 3; i++)
		ASSERT_EQ(RSD_SENDQ_OK, push_v(&q, 10u + (uint32_t)i));
	ASSERT_EQ(RSD_SENDQ_SLOTS, q.count);

	int videos_before = q.count - 2;
	ASSERT_EQ(RSD_SENDQ_OK, push_a(&q, 500));

	ASSERT_EQ(RSD_SENDQ_SLOTS, q.count); /* one out, one in */
	ASSERT_EQ(1u, q.drop_audio);
	ASSERT_EQ(0u, q.drop_video);

	uint8_t types[RSD_SENDQ_SLOTS];
	uint32_t ts[RSD_SENDQ_SLOTS];
	int n = snapshot(&q, types, ts, RSD_SENDQ_SLOTS);
	int videos = 0, audios = 0;
	bool saw_evicted = false, saw_new = false;
	for (int i = 0; i < n; i++) {
		if (types[i] == RSD_FRAME_VIDEO)
			videos++;
		else {
			audios++;
			if (ts[i] == 2)
				saw_evicted = true;
			if (ts[i] == 500)
				saw_new = true;
		}
	}
	ASSERT_EQ(videos_before, videos); /* video untouched */
	ASSERT_EQ(2, audios);
	ASSERT_FALSE(saw_evicted); /* oldest audio (ts=2) evicted */
	ASSERT(saw_new);

	rsd_sendq_destroy(&q);
	PASS();
}

TEST audio_overflow_all_video_drops_incoming(void)
{
	rsd_sendq_t q;
	ASSERT_EQ(0, rsd_sendq_init(&q));

	for (int i = 0; i < RSD_SENDQ_SLOTS; i++)
		ASSERT_EQ(RSD_SENDQ_OK, push_v(&q, (uint32_t)i));
	ASSERT_EQ(RSD_SENDQ_DROPPED, push_a(&q, 500));

	ASSERT_EQ(RSD_SENDQ_SLOTS, q.count); /* video never evicted */
	uint8_t types[RSD_SENDQ_SLOTS];
	uint32_t ts[RSD_SENDQ_SLOTS];
	int n = snapshot(&q, types, ts, RSD_SENDQ_SLOTS);
	for (int i = 0; i < n; i++)
		ASSERT_EQ(RSD_FRAME_VIDEO, types[i]);

	rsd_sendq_destroy(&q);
	PASS();
}

TEST take_audio_from_middle_preserves_rest(void)
{
	rsd_sendq_t q;
	ASSERT_EQ(0, rsd_sendq_init(&q));

	ASSERT_EQ(RSD_SENDQ_OK, push_v(&q, 1));
	ASSERT_EQ(RSD_SENDQ_OK, push_a(&q, 2));
	ASSERT_EQ(RSD_SENDQ_OK, push_v(&q, 3));
	ASSERT_EQ(RSD_SENDQ_OK, push_a(&q, 4));

	rsd_sendq_entry_t out;
	pthread_mutex_lock(&q.lock);
	ASSERT(rsd_sendq_take_audio_locked(&q, &out));
	pthread_mutex_unlock(&q.lock);
	ASSERT_EQ(RSD_FRAME_AUDIO, out.type);
	ASSERT_EQ(2u, out.rtp_ts); /* oldest audio, not newest */
	rsd_sendq_release_entry(&out);

	uint8_t types[8];
	uint32_t ts[8];
	ASSERT_EQ(3, snapshot(&q, types, ts, 8));
	ASSERT_EQ(1u, ts[0]);
	ASSERT_EQ(3u, ts[1]);
	ASSERT_EQ(4u, ts[2]);

	rsd_sendq_destroy(&q);
	PASS();
}

TEST sei_spliced_before_first_vcl(void)
{
	rsd_sendq_t q;
	ASSERT_EQ(0, rsd_sendq_init(&q));

	/* Annex B: SPS (0x67) then IDR (0x65). */
	static const uint8_t frame[] = { 0, 0, 0, 1, 0x67, 0x42, 0, 0, 0, 1, 0x65, 0x88 };
	static const uint8_t sei[] = { 0, 0, 0, 1, 0x06, 0x05, 0x02, 0xde, 0xad };

	ASSERT_EQ(RSD_SENDQ_OK,
		  rsd_sendq_push_video(&q, frame, sizeof(frame), 77, sei, sizeof(sei), false));
	ASSERT_EQ(1, q.count);

	const rsd_sendq_entry_t *e = &q.entries[q.tail];
	ASSERT_EQ(sizeof(frame) + sizeof(sei), e->len);
	/* SPS first (start code + 2 bytes), then the SEI, then the IDR. */
	ASSERT_EQ(0, memcmp(e->data, frame, 6));
	ASSERT_EQ(0, memcmp(e->data + 6, sei, sizeof(sei)));
	ASSERT_EQ(0, memcmp(e->data + 6 + sizeof(sei), frame + 6, sizeof(frame) - 6));

	rsd_sendq_destroy(&q);
	PASS();
}

TEST shutdown_rejects_pushes(void)
{
	rsd_sendq_t q;
	ASSERT_EQ(0, rsd_sendq_init(&q));
	q.shutdown = true;
	ASSERT_EQ(-1, push_v(&q, 1));
	ASSERT_EQ(-1, push_a(&q, 2));
	ASSERT_EQ(0, q.count);
	rsd_sendq_destroy(&q);
	PASS();
}

TEST destroy_frees_pending_entries(void)
{
	rsd_sendq_t q;
	ASSERT_EQ(0, rsd_sendq_init(&q));
	for (int i = 0; i < 5; i++) {
		ASSERT_EQ(RSD_SENDQ_OK, push_v(&q, (uint32_t)i));
		ASSERT_EQ(RSD_SENDQ_OK, push_a(&q, 100u + (uint32_t)i));
	}
	/* Leak check is ASan's job at process exit. */
	rsd_sendq_destroy(&q);
	PASS();
}

SUITE(sendq_suite)
{
	RUN_TEST(push_pop_order_preserved);
	RUN_TEST(video_overflow_keeps_ordered_audio);
	RUN_TEST(video_overflow_all_video_empties);
	RUN_TEST(audio_overflow_evicts_one_oldest_audio);
	RUN_TEST(audio_overflow_all_video_drops_incoming);
	RUN_TEST(take_audio_from_middle_preserves_rest);
	RUN_TEST(sei_spliced_before_first_vcl);
	RUN_TEST(shutdown_rejects_pushes);
	RUN_TEST(destroy_frees_pending_entries);
}

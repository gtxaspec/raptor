#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "greatest.h"
#include "../rwd/rwd_sendq.h"

static const uint8_t frame[] = {0x00, 0x00, 0x00, 0x01, 0x65};

TEST rwd_sendq_preserves_capture_reference(void)
{
	rwd_sendq_t q;
	rwd_sendq_entry_t entry;

	rwd_sendq_init(&q);
	ASSERT_EQ(0, rwd_sendq_push(&q, frame, sizeof(frame), 9000, 1234567, true));
	ASSERT(rwd_sendq_pop(&q, &entry));
	ASSERT_EQ(sizeof(frame), entry.len);
	ASSERT_EQ(9000, entry.rtp_ts);
	ASSERT_EQ(1234567, entry.capture_us);
	ASSERT_EQ(0, memcmp(frame, entry.data, sizeof(frame)));
	free(entry.data);
	rwd_sendq_destroy(&q);
	PASS();
}

TEST rwd_sendq_failure_resumes_only_on_keyframe(void)
{
	rwd_sendq_t q;
	rwd_sendq_entry_t entry;

	rwd_sendq_init(&q);
	ASSERT_EQ(0, rwd_sendq_push(&q, frame, sizeof(frame), 9000, 1000, true));
	ASSERT(rwd_sendq_pop(&q, &entry));
	free(entry.data);
	ASSERT_EQ(0, rwd_sendq_push(&q, frame, sizeof(frame), 12000, 2000, false));

	rwd_sendq_fail(&q);
	ASSERT_EQ(0, q.count);
	ASSERT(q.needs_keyframe);
	ASSERT_EQ(1, rwd_sendq_push(&q, frame, sizeof(frame), 15000, 3000, false));
	ASSERT_EQ(0, q.count);
	ASSERT_EQ(0, rwd_sendq_push(&q, frame, sizeof(frame), 18000, 4000, true));
	ASSERT_FALSE(q.needs_keyframe);
	ASSERT(rwd_sendq_pop(&q, &entry));
	ASSERT_EQ(18000, entry.rtp_ts);
	free(entry.data);
	rwd_sendq_destroy(&q);
	PASS();
}

TEST rwd_sendq_overflow_purges_backlog(void)
{
	rwd_sendq_t q;
	rwd_sendq_entry_t entry;

	rwd_sendq_init(&q);
	for (int i = 0; i < RWD_SENDQ_SLOTS; i++)
		ASSERT_EQ(0, rwd_sendq_push(&q, frame, sizeof(frame), (uint32_t)i, i, true));
	ASSERT_EQ(RWD_SENDQ_SLOTS, q.count);
	ASSERT_EQ(1, rwd_sendq_push(&q, frame, sizeof(frame), 100, 100, false));
	ASSERT_EQ(0, q.count);
	ASSERT(q.needs_keyframe);
	ASSERT_EQ(1, rwd_sendq_push(&q, frame, sizeof(frame), 101, 101, false));
	ASSERT_EQ(0, rwd_sendq_push(&q, frame, sizeof(frame), 102, 102, true));
	ASSERT(rwd_sendq_pop(&q, &entry));
	ASSERT_EQ(102, entry.rtp_ts);
	free(entry.data);
	rwd_sendq_destroy(&q);
	PASS();
}

SUITE(rwd_sendq_suite)
{
	RUN_TEST(rwd_sendq_preserves_capture_reference);
	RUN_TEST(rwd_sendq_failure_resumes_only_on_keyframe);
	RUN_TEST(rwd_sendq_overflow_purges_backlog);
}

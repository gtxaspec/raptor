/*
 * test_ivs_raw.c -- save raw on the JZDL channel: the request/serve handoff
 */

#include <pthread.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "greatest.h"
#include "../rvd/rvd_ivs_raw.h"

static int64_t mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

typedef struct {
	rvd_ivs_raw_t *r;
	uint8_t *buf;
	size_t cap;
	uint32_t size, w, h;
	int timeout_ms;
	int ret;
} grab_arg_t;

static void *grab_thread(void *p)
{
	grab_arg_t *a = p;
	a->ret = rvd_ivs_raw_grab(a->r, a->buf, a->cap, &a->size, &a->w, &a->h, a->timeout_ms);
	return NULL;
}

/* What the JZDL thread does with each frame it receives. */
static void serve_until_taken(rvd_ivs_raw_t *r, const void *data, size_t len, uint32_t w,
			      uint32_t h)
{
	for (int i = 0; i < 500 && !rvd_ivs_raw_wanted(r); i++)
		usleep(2000);
	rvd_ivs_raw_serve(r, data, len, w, h);
}

TEST raw_single_request_gets_the_next_frame(void)
{
	rvd_ivs_raw_t r;
	ASSERT_EQ(0, rvd_ivs_raw_init(&r));

	uint8_t frame[64], buf[64];
	for (int i = 0; i < 64; i++)
		frame[i] = (uint8_t)(i * 3);
	grab_arg_t a = {.r = &r, .buf = buf, .cap = sizeof(buf), .timeout_ms = 2000};
	pthread_t t;
	ASSERT_EQ(0, pthread_create(&t, NULL, grab_thread, &a));
	serve_until_taken(&r, frame, sizeof(frame), 8, 5);
	pthread_join(t, NULL);

	ASSERT_EQ(0, a.ret);
	ASSERT_EQ(64u, a.size);
	ASSERT_EQ(8u, a.w);
	ASSERT_EQ(5u, a.h);
	ASSERT_EQ(0, memcmp(buf, frame, 64));
	ASSERT_FALSE(rvd_ivs_raw_wanted(&r));

	/* nothing parked: a frame offered now goes nowhere */
	rvd_ivs_raw_serve(&r, frame, sizeof(frame), 1, 1);
	ASSERT_EQ(8u, a.w);
	rvd_ivs_raw_close(&r);
	PASS();
}

TEST raw_frame_is_clamped_to_the_buffer(void)
{
	rvd_ivs_raw_t r;
	ASSERT_EQ(0, rvd_ivs_raw_init(&r));

	uint8_t frame[64], buf[16 + 8];
	memset(frame, 0xab, sizeof(frame));
	memset(buf, 0, sizeof(buf));
	grab_arg_t a = {.r = &r, .buf = buf, .cap = 16, .timeout_ms = 2000};
	pthread_t t;
	ASSERT_EQ(0, pthread_create(&t, NULL, grab_thread, &a));
	serve_until_taken(&r, frame, sizeof(frame), 8, 8);
	pthread_join(t, NULL);

	ASSERT_EQ(0, a.ret);
	ASSERT_EQ(16u, a.size);
	ASSERT_EQ(0, buf[16]); /* nothing written past cap */
	rvd_ivs_raw_close(&r);
	PASS();
}

TEST raw_times_out_without_a_frame(void)
{
	rvd_ivs_raw_t r;
	ASSERT_EQ(0, rvd_ivs_raw_init(&r));

	uint8_t buf[16] = {0};
	uint32_t size = 7, w = 7, h = 7;
	int64_t t0 = mono_ms();
	int ret = rvd_ivs_raw_grab(&r, buf, sizeof(buf), &size, &w, &h, 100);
	int64_t took = mono_ms() - t0;

	ASSERT_EQ(-1, ret);
	ASSERT(took >= 95 && took < 1500);
	ASSERT_EQ(7u, size); /* untouched on failure */
	ASSERT_FALSE(rvd_ivs_raw_wanted(&r));

	/* the abandoned request must not take a later frame into its dead buffer */
	uint8_t frame[16] = {1};
	rvd_ivs_raw_serve(&r, frame, sizeof(frame), 1, 1);
	ASSERT_EQ(0, buf[0]);
	rvd_ivs_raw_close(&r);
	PASS();
}

TEST raw_second_request_waits_for_the_first(void)
{
	rvd_ivs_raw_t r;
	ASSERT_EQ(0, rvd_ivs_raw_init(&r));

	uint8_t b1[8], b2[8], frame[8];
	memset(frame, 0x5a, sizeof(frame));
	grab_arg_t a1 = {.r = &r, .buf = b1, .cap = sizeof(b1), .timeout_ms = 3000};
	grab_arg_t a2 = {.r = &r, .buf = b2, .cap = sizeof(b2), .timeout_ms = 3000};
	pthread_t t1, t2;
	pthread_create(&t1, NULL, grab_thread, &a1);
	for (int i = 0; i < 500 && !rvd_ivs_raw_wanted(&r); i++)
		usleep(2000);
	pthread_create(&t2, NULL, grab_thread, &a2);
	usleep(50000);
	serve_until_taken(&r, frame, sizeof(frame), 2, 2);
	pthread_join(t1, NULL);
	serve_until_taken(&r, frame, sizeof(frame), 3, 3);
	pthread_join(t2, NULL);

	ASSERT_EQ(0, a1.ret);
	ASSERT_EQ(0, a2.ret);
	ASSERT_EQ(2u, a1.w);
	ASSERT_EQ(3u, a2.w);
	rvd_ivs_raw_close(&r);
	PASS();
}

TEST raw_close_releases_a_waiting_request(void)
{
	rvd_ivs_raw_t r;
	ASSERT_EQ(0, rvd_ivs_raw_init(&r));

	uint8_t buf[16];
	grab_arg_t a = {.r = &r, .buf = buf, .cap = sizeof(buf), .timeout_ms = 10000};
	pthread_t t;
	ASSERT_EQ(0, pthread_create(&t, NULL, grab_thread, &a));
	for (int i = 0; i < 500 && !rvd_ivs_raw_wanted(&r); i++)
		usleep(2000);
	ASSERT(rvd_ivs_raw_wanted(&r));

	int64_t t0 = mono_ms();
	rvd_ivs_raw_close(&r);
	pthread_join(t, NULL);
	ASSERT(mono_ms() - t0 < 2000);
	ASSERT_EQ(-1, a.ret);

	uint32_t size, w, h;
	ASSERT_EQ(-1, rvd_ivs_raw_grab(&r, buf, sizeof(buf), &size, &w, &h, 1000));
	PASS();
}

SUITE(ivs_raw_suite)
{
	RUN_TEST(raw_single_request_gets_the_next_frame);
	RUN_TEST(raw_frame_is_clamped_to_the_buffer);
	RUN_TEST(raw_times_out_without_a_frame);
	RUN_TEST(raw_second_request_waits_for_the_first);
	RUN_TEST(raw_close_releases_a_waiting_request);
}

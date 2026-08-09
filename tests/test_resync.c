#include "greatest.h"
#include "../rad/rad_resync.h"

#include <rss_common.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * The tracker's whole behavior is what it logs, so the legs read the log
 * back. rss_log's file target flushes every line, so a temp file is the
 * cheapest way to see exactly what an operator would see.
 */

#define CAP_MAX 8192

static char cap_path[64];
static char cap_buf[CAP_MAX];

static void cap_begin(void)
{
	snprintf(cap_path, sizeof(cap_path), "/tmp/rad_resync_cap.%d", (int)getpid());
	unlink(cap_path);
	rss_log_init("radtest", RSS_LOG_WARN, RSS_LOG_TARGET_FILE, cap_path);
}

static const char *cap_end(void)
{
	FILE *f = fopen(cap_path, "r");
	size_t n = 0;

	cap_buf[0] = '\0';
	if (f) {
		n = fread(cap_buf, 1, sizeof(cap_buf) - 1, f);
		fclose(f);
	}
	cap_buf[n] = '\0';
	/* Put logging back where the rest of the suite expects it. */
	rss_log_init("tests", RSS_LOG_DEBUG, RSS_LOG_TARGET_STDERR, NULL);
	unlink(cap_path);
	return cap_buf;
}

static int cap_count(const char *hay, const char *needle)
{
	int n = 0;
	const char *p = hay;

	while ((p = strstr(p, needle)) != NULL) {
		n++;
		p += strlen(needle);
	}
	return n;
}

#define S(sec) ((int64_t)(sec) * 1000000LL)
#define MS(ms) ((int64_t)(ms) * 1000LL)

/* A single resync is worth its own line, and must not also be summarised. */
TEST resync_lone_stays_one_line(void)
{
	rad_resync_log_t r = {0};

	cap_begin();
	rad_resync_note(&r, S(10), MS(150));
	/* Long enough after to close the episode. */
	rad_resync_tick(&r, S(10) + RAD_RESYNC_QUIET_US);
	const char *log = cap_end();

	ASSERT_EQ_FMT(1, cap_count(log, "audio clock resync +150ms"), "%d");
	ASSERT_EQ_FMT(0, cap_count(log, "times in"), "%d");
	/* Closed: the counters are clear and the next resync logs in full. */
	ASSERT_EQ_FMT(0, (int)r.count, "%d");
	ASSERT_EQ_FMT(0, (long long)r.open_us == 0 ? 0 : 1, "%d");
	PASS();
}

/* A burst logs once, then one summary naming count, span and totals. */
TEST resync_burst_summarised_once(void)
{
	rad_resync_log_t r = {0};
	int64_t t = S(100);

	cap_begin();
	for (int i = 0; i < 12; i++) {
		rad_resync_tick(&r, t);
		rad_resync_note(&r, t, MS(150));
		t += S(1);
	}
	/* Episode stops; the closing tick must summarise it. */
	rad_resync_tick(&r, t + RAD_RESYNC_QUIET_US);
	const char *log = cap_end();

	ASSERT_EQ_FMT(1, cap_count(log, "audio clock resync +150ms"), "%d");
	ASSERT_EQ_FMT(1, cap_count(log, "times in"), "%d");
	/* 12 resyncs spanning 11s, 150ms each, all in one direction. */
	ASSERT(strstr(log, "resynced 12 times in 11s: net +1800ms, absolute 1800ms") != NULL);
	PASS();
}

/* An episode longer than a minute reports as it runs, cumulatively. */
TEST resync_long_episode_reports_each_minute(void)
{
	rad_resync_log_t r = {0};
	int64_t t = S(1000);

	cap_begin();
	/* A resync every second for 130s: two periodic reports, then a close. */
	for (int i = 0; i < 130; i++) {
		rad_resync_tick(&r, t);
		rad_resync_note(&r, t, MS(150));
		t += S(1);
	}
	rad_resync_tick(&r, t + RAD_RESYNC_QUIET_US);
	const char *log = cap_end();

	/* Two minute reports plus the closing one -- not one per chunk. */
	ASSERT_EQ_FMT(3, cap_count(log, "times in"), "%d");
	/* Cumulative, so the counts rise and the last covers the episode. */
	ASSERT(strstr(log, "resynced 60 times in 59s") != NULL);
	ASSERT(strstr(log, "resynced 120 times in 119s") != NULL);
	ASSERT(strstr(log, "resynced 130 times in 129s") != NULL);
	PASS();
}

/*
 * The leg the reviewed shape failed. A periodic report used to reset the
 * counters while leaving the window open, so a lone resync arriving after
 * one was counted into a fresh accumulator and then dropped by the
 * count-of-one guard -- never logged in full, never summarised.
 */
TEST resync_tail_after_periodic_report_is_reported(void)
{
	rad_resync_log_t r = {0};
	int64_t t = S(2000);

	cap_begin();
	/*
	 * 61 seconds of resyncs: the tick at the 60s mark reports, and the
	 * resync right behind it is the only one after that report.
	 */
	for (int i = 0; i <= 60; i++) {
		rad_resync_tick(&r, t);
		rad_resync_note(&r, t, MS(150));
		t += S(1);
	}
	rad_resync_tick(&r, t - S(1) + RAD_RESYNC_QUIET_US);
	const char *log = cap_end();

	/* The periodic report covers 60; the closing one must cover all 61. */
	ASSERT(strstr(log, "resynced 60 times in 59s") != NULL);
	ASSERT(strstr(log, "resynced 61 times in 60s") != NULL);
	ASSERT_EQ_FMT(2, cap_count(log, "times in"), "%d");
	PASS();
}

/* Corrections in both directions cancel in the net, so report magnitude too. */
TEST resync_alternating_signs_show_in_absolute(void)
{
	rad_resync_log_t r = {0};
	int64_t t = S(3000);

	cap_begin();
	for (int i = 0; i < 10; i++) {
		rad_resync_tick(&r, t);
		rad_resync_note(&r, t, i % 2 ? MS(-1000) : MS(1000));
		t += S(1);
	}
	rad_resync_tick(&r, t + RAD_RESYNC_QUIET_US);
	const char *log = cap_end();

	/* Net cancels to zero; absolute says 10s of audio was moved. */
	ASSERT(strstr(log, "resynced 10 times in 9s: net +0ms, absolute 10000ms") != NULL);
	PASS();
}

/* A quiet tick on a closed tracker must not log or reopen anything. */
TEST resync_tick_while_idle_is_silent(void)
{
	rad_resync_log_t r = {0};

	cap_begin();
	for (int i = 0; i < 100; i++)
		rad_resync_tick(&r, S(4000) + S(i));
	const char *log = cap_end();

	ASSERT_EQ_FMT(0, (int)strlen(log), "%d");
	PASS();
}

SUITE(resync_suite)
{
	RUN_TEST(resync_lone_stays_one_line);
	RUN_TEST(resync_burst_summarised_once);
	RUN_TEST(resync_long_episode_reports_each_minute);
	RUN_TEST(resync_tail_after_periodic_report_is_reported);
	RUN_TEST(resync_alternating_signs_show_in_absolute);
	RUN_TEST(resync_tick_while_idle_is_silent);
}

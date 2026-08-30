#include "greatest.h"
#include "../rmr/rmr_storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static bool same_dev_as_root(const char *path)
{
	struct stat a, b;
	if (stat("/", &a) != 0 || stat(path, &b) != 0)
		return true;
	return a.st_dev == b.st_dev;
}

/* Wall-clock-aligned rotation: pure time math, times injected. */

TEST storage_boundary_from_mid_period(void)
{
	/* 12:03:37 UTC with 1-minute segments -> 12:04:00 */
	int64_t now = ((int64_t)12 * 3600 + 3 * 60 + 37) * 1000000LL;
	ASSERT_EQ_FMT((long long)(((int64_t)12 * 3600 + 4 * 60) * 1000000LL),
		      (long long)rmr_storage_next_boundary(now, 60), "%lld");
	/* Same instant with 5-minute segments -> 12:05:00 */
	ASSERT_EQ_FMT((long long)(((int64_t)12 * 3600 + 5 * 60) * 1000000LL),
		      (long long)rmr_storage_next_boundary(now, 300), "%lld");
	PASS();
}

TEST storage_boundary_on_the_boundary(void)
{
	/* Exactly on a boundary the next one is a full period away. */
	int64_t now = ((int64_t)12 * 3600 + 4 * 60) * 1000000LL;
	ASSERT_EQ_FMT((long long)(((int64_t)12 * 3600 + 5 * 60) * 1000000LL),
		      (long long)rmr_storage_next_boundary(now, 60), "%lld");
	PASS();
}

TEST storage_rotate_at_wall_boundary(void)
{
	char dir[] = "/tmp/rmr_storage_test_XXXXXX";
	ASSERT(mkdtemp(dir));
	rmr_storage_config_t cfg = {.base_path = dir, .segment_minutes = 1};
	rmr_storage_t *st = rmr_storage_create(&cfg);
	ASSERT(st);

	int64_t start = ((int64_t)12 * 3600 + 3 * 60 + 37) * 1000000LL;
	/* Before the boundary: no rotation. */
	ASSERT_EQ(false, rmr_storage_should_rotate_at(st, start, start + 10 * 1000000LL));
	/* Past 12:04:00: rotate. */
	ASSERT_EQ(true, rmr_storage_should_rotate_at(st, start, start + 24 * 1000000LL));
	rmr_storage_destroy(st);
	rmdir(dir);
	PASS();
}

TEST storage_rotate_minimum_floor(void)
{
	/* A segment opened just before the boundary (or a backward NTP
	 * step) must not produce micro-clips: floor of 5s. */
	char dir[] = "/tmp/rmr_storage_test_XXXXXX";
	ASSERT(mkdtemp(dir));
	rmr_storage_config_t cfg = {.base_path = dir, .segment_minutes = 1};
	rmr_storage_t *st = rmr_storage_create(&cfg);
	ASSERT(st);

	int64_t start = ((int64_t)12 * 3600 + 3 * 60 + 59) * 1000000LL;
	/* 2s in, boundary crossed, but under the floor: hold. */
	ASSERT_EQ(false, rmr_storage_should_rotate_at(st, start, start + 2 * 1000000LL));
	/* 6s in: rotate (boundary long crossed, floor satisfied). */
	ASSERT_EQ(true, rmr_storage_should_rotate_at(st, start, start + 6 * 1000000LL));
	rmr_storage_destroy(st);
	rmdir(dir);
	PASS();
}

TEST storage_rotate_ntp_forward_step(void)
{
	/* A forward wall-clock step mid-segment lands past the boundary:
	 * rotate on the next check, do not wedge. */
	char dir[] = "/tmp/rmr_storage_test_XXXXXX";
	ASSERT(mkdtemp(dir));
	rmr_storage_config_t cfg = {.base_path = dir, .segment_minutes = 1};
	rmr_storage_t *st = rmr_storage_create(&cfg);
	ASSERT(st);

	int64_t start = ((int64_t)12 * 3600 + 3 * 60 + 10) * 1000000LL;
	int64_t stepped = start + 300 * 1000000LL; /* +5min jump */
	ASSERT_EQ(true, rmr_storage_should_rotate_at(st, start, stepped));
	rmr_storage_destroy(st);
	rmdir(dir);
	PASS();
}

TEST storage_segment_seconds_override(void)
{
	/* segment_seconds (testing/debug knob) overrides minutes. */
	int64_t now = ((int64_t)12 * 3600 + 3 * 60 + 37) * 1000000LL;
	ASSERT_EQ_FMT((long long)(((int64_t)12 * 3600 + 3 * 60 + 40) * 1000000LL),
		      (long long)rmr_storage_next_boundary(now, 10), "%lld");
	PASS();
}

TEST storage_available_existing_dir(void)
{
	char dir[] = "/tmp/rmr_storage_test_XXXXXX";
	ASSERT(mkdtemp(dir));

	rmr_storage_config_t cfg = {.base_path = dir, .segment_minutes = 5};
	rmr_storage_t *st = rmr_storage_create(&cfg);
	ASSERT(st);
	ASSERT(rmr_storage_available(st));

	rmr_storage_destroy(st);
	rmdir(dir);
	PASS();
}

TEST storage_autocreate_on_mounted_fs(void)
{
	/* /dev/shm is tmpfs on Linux — a different filesystem than "/",
	 * so auto-creation must kick in for the missing directory. */
	if (same_dev_as_root("/dev/shm"))
		SKIP();

	char base[64];
	snprintf(base, sizeof(base), "/dev/shm/rmr_st_%d/sub", getpid());

	rmr_storage_config_t cfg = {.base_path = base, .segment_minutes = 5};
	rmr_storage_t *st = rmr_storage_create(&cfg);
	ASSERT(st);
	ASSERT(rmr_storage_available(st));
	ASSERT_EQ(0, access(base, W_OK));

	rmr_storage_destroy(st);
	rmdir(base);
	*strrchr(base, '/') = '\0';
	rmdir(base);
	PASS();
}

TEST storage_refuses_rootfs_autocreate(void)
{
	/* /var/tmp sits on the root filesystem on typical hosts; skip
	 * when it does not so the test stays environment-independent. */
	if (!same_dev_as_root("/var/tmp"))
		SKIP();

	char base[64];
	snprintf(base, sizeof(base), "/var/tmp/rmr_st_refuse_%d", getpid());

	rmr_storage_config_t cfg = {.base_path = base, .segment_minutes = 5};
	rmr_storage_t *st = rmr_storage_create(&cfg);
	ASSERT(st);
	ASSERT_FALSE(rmr_storage_available(st));
	ASSERT(access(base, F_OK) != 0); /* nothing was created */

	rmr_storage_destroy(st);
	PASS();
}

TEST storage_publishes_final_name_on_close(void)
{
	char dir[] = "/tmp/rmr_storage_test_XXXXXX";
	ASSERT(mkdtemp(dir));

	rmr_storage_config_t cfg = {.base_path = dir, .segment_minutes = 5};
	rmr_storage_t *st = rmr_storage_create(&cfg);
	ASSERT(st);

	char path[256];
	int fd = rmr_storage_open_segment(st, path, sizeof(path));
	ASSERT(fd >= 0);

	char tmp[300];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	ASSERT_EQ(4, (int)write(fd, "data", 4));

	/* In flight: only the .tmp twin exists. */
	ASSERT(access(tmp, F_OK) == 0);
	ASSERT(access(path, F_OK) != 0);

	rmr_storage_close_segment(fd, path, 4);

	/* Published: .mp4 with exactly the bytes written. */
	struct stat s;
	ASSERT(stat(path, &s) == 0);
	ASSERT_EQ(4, (int)s.st_size);
	ASSERT(access(tmp, F_OK) != 0);

	rmr_storage_destroy(st);
	snprintf(tmp, sizeof(tmp), "%s", path);
	*strrchr(tmp, '/') = '\0';
	rmdir(tmp);
	rmdir(dir);
	PASS();
}

TEST storage_discards_empty_segment(void)
{
	char dir[] = "/tmp/rmr_storage_test_XXXXXX";
	ASSERT(mkdtemp(dir));

	rmr_storage_config_t cfg = {.base_path = dir, .segment_minutes = 5};
	rmr_storage_t *st = rmr_storage_create(&cfg);
	ASSERT(st);

	char path[256];
	int fd = rmr_storage_open_segment(st, path, sizeof(path));
	ASSERT(fd >= 0);

	char tmp[300];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	ASSERT(access(tmp, F_OK) == 0);

	rmr_storage_close_segment(fd, path, 0);

	/* Nothing written: no .mp4, no .tmp. */
	ASSERT(access(path, F_OK) != 0);
	ASSERT(access(tmp, F_OK) != 0);

	rmr_storage_destroy(st);
	rmdir(dir);
	PASS();
}

TEST storage_prealloc_shrinks_on_close(void)
{
	char dir[] = "/tmp/rmr_storage_test_XXXXXX";
	ASSERT(mkdtemp(dir));

	rmr_storage_config_t cfg = {.base_path = dir, .segment_minutes = 5, .prealloc_bytes = 4096};
	rmr_storage_t *st = rmr_storage_create(&cfg);
	ASSERT(st);

	char path[256];
	int fd = rmr_storage_open_segment(st, path, sizeof(path));
	ASSERT(fd >= 0);

	/* Reservation is live while recording (metadata-only growth). */
	char tmp[300];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	struct stat s;
	ASSERT(stat(tmp, &s) == 0);
	ASSERT_EQ(4096, (int)s.st_size);

	ASSERT_EQ(7, (int)write(fd, "payload", 7));
	rmr_storage_close_segment(fd, path, 7);

	/* Shrunk to the written size at publication. */
	ASSERT(stat(path, &s) == 0);
	ASSERT_EQ(7, (int)s.st_size);

	rmr_storage_destroy(st);
	snprintf(tmp, sizeof(tmp), "%s", path);
	*strrchr(tmp, '/') = '\0';
	rmdir(tmp);
	rmdir(dir);
	PASS();
}

TEST storage_sweeps_stale_tmp_on_first_use(void)
{
	char dir[] = "/tmp/rmr_storage_test_XXXXXX";
	ASSERT(mkdtemp(dir));

	/* Simulate an unclean shutdown: a date dir with a leftover .tmp
	 * plus a clean .mp4 that must survive. */
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	char day[320], good[512], stale[512];
	snprintf(day, sizeof(day), "%s/%04d-%02d-%02d", dir, tm.tm_year + 1900, tm.tm_mon + 1,
		 tm.tm_mday);
	ASSERT(mkdir(day, 0755) == 0);
	snprintf(good, sizeof(good), "%s/10-00-00.mp4", day);
	snprintf(stale, sizeof(stale), "%s/11-00-00.mp4.tmp", day);
	FILE *g = fopen(good, "w"), *s = fopen(stale, "w");
	ASSERT(good != NULL && g != NULL);
	ASSERT(stale != NULL && s != NULL);
	fclose(g);
	fclose(s);

	rmr_storage_config_t cfg = {.base_path = dir, .segment_minutes = 5};
	rmr_storage_t *st = rmr_storage_create(&cfg);
	ASSERT(st);
	ASSERT(rmr_storage_available(st)); /* triggers the sweep */

	ASSERT(access(stale, F_OK) != 0); /* gone */
	ASSERT(access(good, F_OK) == 0);  /* kept */

	rmr_storage_destroy(st);
	unlink(good);
	rmdir(day);
	rmdir(dir);
	PASS();
}

SUITE(storage_suite)
{
	RUN_TEST(storage_boundary_from_mid_period);
	RUN_TEST(storage_boundary_on_the_boundary);
	RUN_TEST(storage_rotate_at_wall_boundary);
	RUN_TEST(storage_rotate_minimum_floor);
	RUN_TEST(storage_rotate_ntp_forward_step);
	RUN_TEST(storage_segment_seconds_override);
	RUN_TEST(storage_available_existing_dir);
	RUN_TEST(storage_autocreate_on_mounted_fs);
	RUN_TEST(storage_refuses_rootfs_autocreate);
	RUN_TEST(storage_publishes_final_name_on_close);
	RUN_TEST(storage_discards_empty_segment);
	RUN_TEST(storage_prealloc_shrinks_on_close);
	RUN_TEST(storage_sweeps_stale_tmp_on_first_use);
}

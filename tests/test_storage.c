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

/* 2026-11-22 16:59:00 local time, as the daemon would stamp it */
static int64_t local_rt_us(int h, int m, int sec, int day)
{
	struct tm tm = {.tm_year = 126,
			.tm_mon = 10,
			.tm_mday = day,
			.tm_hour = h,
			.tm_min = m,
			.tm_sec = sec,
			.tm_isdst = -1};
	return (int64_t)mktime(&tm) * 1000000LL;
}

TEST storage_open_file_suffix(void)
{
	char dir[] = "/tmp/rmr_storage_test_XXXXXX";
	ASSERT(mkdtemp(dir));
	rmr_storage_config_t cfg = {.base_path = dir, .segment_minutes = 1};
	rmr_storage_t *st = rmr_storage_create(&cfg);
	ASSERT(st);

	int64_t rt = local_rt_us(16, 59, 0, 22);
	char front[256], rear[256], want[300];
	int fd = rmr_storage_open_file(st, rt, NULL, front, sizeof(front));
	ASSERT(fd >= 0);
	close(fd);
	fd = rmr_storage_open_file(st, rt, "R", rear, sizeof(rear));
	ASSERT(fd >= 0);
	close(fd);
	snprintf(want, sizeof(want), "%s/2026-11-22/16-59-00.mp4", dir);
	ASSERT_STR_EQ(want, front);
	snprintf(want, sizeof(want), "%s/2026-11-22/16-59-00_R.mp4", dir);
	ASSERT_STR_EQ(want, rear);
	ASSERT_EQ(0, access(front, F_OK));
	ASSERT_EQ(0, access(rear, F_OK));

	unlink(front);
	unlink(rear);
	*strrchr(front, '/') = '\0';
	rmdir(front);
	rmr_storage_destroy(st);
	rmdir(dir);
	PASS();
}

TEST storage_name_stamp_pairs_tracks(void)
{
	rmr_name_stamp_t s = {0};
	uint32_t front = 0, rear = 0;
	/* front opens at :00.1 for the period ending 17:00, rear a frame later */
	ASSERT_EQ(100, rmr_name_stamp_take(&s, &front, 1000, 100, 5000));
	ASSERT_EQ(100, rmr_name_stamp_take(&s, &rear, 1040, 140, 5000));
	/* the next period: whichever track rotates first names the pair */
	ASSERT_EQ(5010, rmr_name_stamp_take(&s, &rear, 5010, 5010, 9000));
	ASSERT_EQ(5010, rmr_name_stamp_take(&s, &front, 5300, 5300, 9000));
	PASS();
}

TEST storage_name_stamp_never_reuses_own(void)
{
	rmr_name_stamp_t s = {0};
	uint32_t front = 0, rear = 0;
	ASSERT_EQ(100, rmr_name_stamp_take(&s, &front, 100, 100, 5000));
	/* pause/resume in the same period: a fresh name, never the file
	 * just closed (that open truncates) */
	ASSERT_EQ(900, rmr_name_stamp_take(&s, &front, 900, 900, 5000));
	/* the rear, never opened under the first stamp, pairs the new one */
	ASSERT_EQ(900, rmr_name_stamp_take(&s, &rear, 950, 950, 5000));
	/* past its period a stamp is not shared */
	uint32_t late = 0;
	ASSERT_EQ(6000, rmr_name_stamp_take(&s, &late, 6000, 6000, 9000));
	PASS();
}

static void make_file(const char *dir, const char *day, const char *name, off_t size)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", dir, day);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/%s/%s", dir, day, name);
	FILE *f = fopen(path, "w");
	if (f) {
		if (ftruncate(fileno(f), size) != 0)
			fputc(0, f);
		fclose(f);
	}
}

static bool file_exists(const char *dir, const char *day, const char *name)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s/%s", dir, day, name);
	return access(path, F_OK) == 0;
}

TEST storage_enforce_deletes_pairs_together(void)
{
	char dir[] = "/tmp/rmr_storage_test_XXXXXX";
	ASSERT(mkdtemp(dir));
	rmr_storage_config_t cfg = {.base_path = dir, .segment_minutes = 1, .max_storage_mb = 3};
	rmr_storage_t *st = rmr_storage_create(&cfg);
	ASSERT(st);

	const off_t mb = 1024 * 1024;
	make_file(dir, "2026-11-21", "23-59-00.mp4", mb);
	make_file(dir, "2026-11-21", "23-59-00_R.mp4", mb);
	make_file(dir, "2026-11-22", "00-00-00.mp4", mb);
	make_file(dir, "2026-11-22", "00-00-00_R.mp4", mb / 2);

	ASSERT_EQ(2, rmr_storage_enforce_limit(st));
	ASSERT_FALSE(file_exists(dir, "2026-11-21", "23-59-00.mp4"));
	ASSERT_FALSE(file_exists(dir, "2026-11-21", "23-59-00_R.mp4"));
	ASSERT(file_exists(dir, "2026-11-22", "00-00-00.mp4"));
	ASSERT(file_exists(dir, "2026-11-22", "00-00-00_R.mp4"));
	/* under the quota now: nothing more goes */
	ASSERT_EQ(0, rmr_storage_enforce_limit(st));

	char path[512];
	snprintf(path, sizeof(path), "%s/2026-11-22/00-00-00.mp4", dir);
	unlink(path);
	snprintf(path, sizeof(path), "%s/2026-11-22/00-00-00_R.mp4", dir);
	unlink(path);
	snprintf(path, sizeof(path), "%s/2026-11-22", dir);
	rmdir(path);
	rmr_storage_destroy(st);
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
	RUN_TEST(storage_open_file_suffix);
	RUN_TEST(storage_name_stamp_pairs_tracks);
	RUN_TEST(storage_name_stamp_never_reuses_own);
	RUN_TEST(storage_enforce_deletes_pairs_together);
}

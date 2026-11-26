#include "greatest.h"
#include "rmr_io.h"

#include <rss_common.h>

#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int tmp_fd(char *path, size_t cap)
{
	snprintf(path, cap, "/tmp/rmr_io_test_XXXXXX");
	return mkstemp(path);
}

static size_t slurp(const char *path, uint8_t *buf, size_t cap)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return 0;
	size_t n = fread(buf, 1, cap, f);
	fclose(f);
	return n;
}

/* Link-time wraps (tests/Makefile): an SD card's slow flushes, on demand */
int __real_fdatasync(int fd);
int __real_fsync(int fd);
static int sync_delay_ms;
static int sync_fail_fd = -1;
static int sync_count;
static int sync_log_n;
static struct {
	char kind;
	int fd;
} sync_log[64];
static pthread_mutex_t sync_log_lock = PTHREAD_MUTEX_INITIALIZER;

static void log_sync(char kind, int fd)
{
	pthread_mutex_lock(&sync_log_lock);
	if (sync_log_n < 64)
		sync_log[sync_log_n++] = (typeof(sync_log[0])){kind, fd};
	sync_count++;
	pthread_mutex_unlock(&sync_log_lock);
}

int __wrap_fdatasync(int fd)
{
	if (sync_delay_ms)
		usleep((useconds_t)sync_delay_ms * 1000);
	log_sync('d', fd);
	if (fd == sync_fail_fd) {
		errno = EIO;
		return -1;
	}
	return __real_fdatasync(fd);
}

int __wrap_fsync(int fd)
{
	if (sync_delay_ms)
		usleep((useconds_t)sync_delay_ms * 1000);
	log_sync('f', fd);
	if (fd == sync_fail_fd) {
		errno = EIO;
		return -1;
	}
	return __real_fsync(fd);
}

static void sync_reset(int delay_ms)
{
	sync_delay_ms = delay_ms;
	sync_fail_fd = -1;
	sync_count = 0;
	sync_log_n = 0;
}

static int64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int count_in_file(const char *path, const char *needle)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	char line[512];
	int n = 0;
	while (fgets(line, sizeof(line), f))
		n += strstr(line, needle) != NULL;
	fclose(f);
	return n;
}

/* Close fd through the writer with the log in a file; the number of
 * close-time fsync warnings it logged */
static int close_and_count_warnings(int fd, const char *path, bool fail)
{
	char log[96];
	snprintf(log, sizeof(log), "%s.log", path);
	rss_log_init("test", RSS_LOG_WARN, RSS_LOG_TARGET_FILE, log);
	rmr_io_t *io = rmr_io_create(64 * 1024);
	if (!io)
		return -1;
	sync_reset(0);
	if (fail)
		sync_fail_fd = fd;
	rmr_io_write(io, fd, "x", 1);
	rmr_io_close(io, fd);
	rmr_io_drain(io);
	rmr_io_destroy(io);
	sync_reset(0);
	rss_log_init("test", RSS_LOG_INFO, RSS_LOG_TARGET_STDERR, NULL);
	int n = count_in_file(log, "fsync at close failed");
	unlink(log);
	return n;
}

TEST io_order_across_wraps(void)
{
	/* A 64 KB ring takes 1 MB in 3001-byte pieces: every byte in order */
	char path[64];
	int fd = tmp_fd(path, sizeof(path));
	ASSERT(fd >= 0);
	rmr_io_t *io = rmr_io_create(64 * 1024);
	ASSERT(io);
	uint8_t chunk[3001];
	uint32_t total = 0;
	for (int i = 0; total < 1024 * 1024; i++) {
		for (size_t k = 0; k < sizeof(chunk); k++)
			chunk[k] = (uint8_t)((total + k) * 7);
		ASSERT_EQ(0, rmr_io_write(io, fd, chunk, sizeof(chunk)));
		total += sizeof(chunk);
	}
	rmr_io_close(io, fd);
	rmr_io_drain(io);
	ASSERT(rmr_io_peak(io) <= 64 * 1024);
	rmr_io_destroy(io);

	uint8_t *back = malloc(total);
	ASSERT(back);
	ASSERT_EQ(total, (uint32_t)slurp(path, back, total));
	for (uint32_t k = 0; k < total; k++)
		if (back[k] != (uint8_t)(k * 7))
			FAILm("byte out of order");
	free(back);
	unlink(path);
	PASS();
}

TEST io_pwrite_after_appends(void)
{
	char path[64];
	int fd = tmp_fd(path, sizeof(path));
	ASSERT(fd >= 0);
	rmr_io_t *io = rmr_io_create(64 * 1024);
	ASSERT(io);
	ASSERT_EQ(0, rmr_io_write(io, fd, "AAAAAAAAAA", 10));
	ASSERT_EQ(0, rmr_io_pwrite(io, fd, "bb", 2, 3));
	ASSERT_EQ(0, rmr_io_write(io, fd, "CC", 2));
	rmr_io_sync(io, fd);
	rmr_io_close(io, fd);
	rmr_io_destroy(io);
	uint8_t back[32];
	ASSERT_EQ(12u, (uint32_t)slurp(path, back, sizeof(back)));
	ASSERT_MEM_EQ("AAAbbAAAAACC", back, 12);
	unlink(path);
	PASS();
}

TEST io_write_larger_than_ring(void)
{
	char path[64];
	int fd = tmp_fd(path, sizeof(path));
	ASSERT(fd >= 0);
	rmr_io_t *io = rmr_io_create(64 * 1024);
	ASSERT(io);
	uint32_t n = 200 * 1024;
	uint8_t *big = malloc(n);
	ASSERT(big);
	for (uint32_t k = 0; k < n; k++)
		big[k] = (uint8_t)(k ^ (k >> 8));
	ASSERT_EQ(0, rmr_io_write(io, fd, big, n));
	rmr_io_close(io, fd);
	rmr_io_destroy(io);
	uint8_t *back = malloc(n);
	ASSERT(back);
	ASSERT_EQ(n, (uint32_t)slurp(path, back, n));
	ASSERT_MEM_EQ(big, back, n);
	free(big);
	free(back);
	unlink(path);
	PASS();
}

TEST io_error_reported(void)
{
	/* A read-only descriptor fails its write; later writes refuse */
	char path[64];
	int fd = tmp_fd(path, sizeof(path));
	ASSERT(fd >= 0);
	close(fd);
	fd = open(path, O_RDONLY);
	ASSERT(fd >= 0);
	rmr_io_t *io = rmr_io_create(64 * 1024);
	ASSERT(io);
	ASSERT_EQ(0, rmr_io_write(io, fd, "x", 1));
	rmr_io_drain(io);
	ASSERT_EQ(EBADF, rmr_io_error(io, fd));
	ASSERT_EQ(-1, rmr_io_write(io, fd, "y", 1));
	rmr_io_close(io, fd);
	rmr_io_drain(io);
	ASSERT_EQ(0, rmr_io_error(io, fd));
	rmr_io_destroy(io);
	unlink(path);
	PASS();
}

TEST io_room_reports_free_space(void)
{
	rmr_io_t *io = rmr_io_create(64 * 1024);
	ASSERT(io);
	ASSERT_EQ(64 * 1024, (int)rmr_io_room(io));
	rmr_io_destroy(io);
	PASS();
}

/* A 300 ms flush must not hold up the writes queued behind it: on a
 * card the writer would otherwise sit out every sync, and the segments
 * drop fragments while it does. */
TEST io_slow_sync_does_not_block_writes(void)
{
	char a[64], b[64];
	int fa = tmp_fd(a, sizeof(a)), fb = tmp_fd(b, sizeof(b));
	ASSERT(fa >= 0 && fb >= 0);
	rmr_io_t *io = rmr_io_create(64 * 1024);
	ASSERT(io);
	sync_reset(300);
	static uint8_t chunk[16 * 1024];
	ASSERT_EQ(0, rmr_io_write(io, fa, chunk, sizeof(chunk)));
	rmr_io_sync(io, fa);
	int64_t t0 = now_ms();
	/* 512 KB through a 64 KB ring: the writer has to keep going */
	for (int i = 0; i < 32; i++)
		ASSERT_EQ(0, rmr_io_write(io, fb, chunk, sizeof(chunk)));
	int64_t queued = now_ms() - t0;
	rmr_io_drain(io);
	int64_t drained = now_ms() - t0;
	int syncs = sync_count;
	sync_reset(0);
	rmr_io_close(io, fa);
	rmr_io_close(io, fb);
	rmr_io_destroy(io);
	unlink(a);
	unlink(b);
	ASSERTm("writes waited on the flush", queued < 150);
	ASSERTm("drain waits for the flush too", drained >= 250);
	ASSERT_EQ(1, syncs);
	PASS();
}

/* Syncs and the close of one file run in the order queued: the file is
 * flushed twice, then fsync'd and closed, never closed under a flush */
TEST io_close_after_its_syncs(void)
{
	char a[64];
	int fd = tmp_fd(a, sizeof(a));
	ASSERT(fd >= 0);
	rmr_io_t *io = rmr_io_create(64 * 1024);
	ASSERT(io);
	sync_reset(50);
	ASSERT_EQ(0, rmr_io_write(io, fd, "a", 1));
	rmr_io_sync(io, fd);
	rmr_io_drain(io); /* the first flush runs before the second is queued */
	ASSERT_EQ(0, rmr_io_write(io, fd, "b", 1));
	rmr_io_sync(io, fd);
	rmr_io_close(io, fd);
	rmr_io_drain(io);
	char seq[8] = "";
	for (int i = 0; i < sync_log_n && i < 7; i++)
		seq[i] = sync_log[i].kind;
	int still_open = fcntl(fd, F_GETFD) != -1;
	sync_reset(0);
	rmr_io_destroy(io);
	uint8_t buf[4];
	size_t n = slurp(a, buf, sizeof(buf));
	unlink(a);
	ASSERT_STR_EQ("ddf", seq);
	ASSERTm("closed after its syncs", !still_open);
	ASSERT_EQ(2, (int)n);
	PASS();
}

/* Syncs piling up behind a slow one coalesce: one flush covers them */
TEST io_syncs_coalesce(void)
{
	char a[64], b[64];
	int fa = tmp_fd(a, sizeof(a)), fb = tmp_fd(b, sizeof(b));
	ASSERT(fa >= 0 && fb >= 0);
	rmr_io_t *io = rmr_io_create(64 * 1024);
	ASSERT(io);
	sync_reset(200);
	rmr_io_sync(io, fa); /* occupies the sync thread */
	usleep(20000);
	for (int i = 0; i < 5; i++) {
		ASSERT_EQ(0, rmr_io_write(io, fb, "x", 1));
		rmr_io_sync(io, fb);
	}
	rmr_io_drain(io);
	int nb = 0;
	for (int i = 0; i < sync_log_n; i++)
		nb += sync_log[i].fd == fb;
	sync_reset(0);
	rmr_io_close(io, fa);
	rmr_io_close(io, fb);
	rmr_io_destroy(io);
	unlink(a);
	unlink(b);
	ASSERT_EQ(1, nb);
	PASS();
}

/* A failed flush fails the file, like a failed write */
TEST io_sync_error_reported(void)
{
	char a[64];
	int fd = tmp_fd(a, sizeof(a));
	ASSERT(fd >= 0);
	rmr_io_t *io = rmr_io_create(64 * 1024);
	ASSERT(io);
	sync_reset(0);
	sync_fail_fd = fd;
	ASSERT_EQ(0, rmr_io_write(io, fd, "x", 1));
	rmr_io_sync(io, fd);
	rmr_io_drain(io);
	int err = rmr_io_error(io, fd);
	int later = rmr_io_write(io, fd, "y", 1);
	sync_reset(0);
	rmr_io_close(io, fd);
	rmr_io_destroy(io);
	unlink(a);
	ASSERT_EQ(EIO, err);
	ASSERT_EQ(-1, later);
	PASS();
}

/* A failed fsync at close is logged once, and the file still closes */
TEST io_close_fsync_failure_still_closes(void)
{
	char a[64];
	int fd = tmp_fd(a, sizeof(a));
	ASSERT(fd >= 0);
	int warns = close_and_count_warnings(fd, a, true);
	int still_open = fcntl(fd, F_GETFD) != -1;
	unlink(a);
	ASSERT_EQ(1, warns);
	ASSERTm("closed despite the failed fsync", !still_open);
	PASS();
}

/* A clean close logs nothing */
TEST io_close_clean_is_quiet(void)
{
	char a[64];
	int fd = tmp_fd(a, sizeof(a));
	ASSERT(fd >= 0);
	int warns = close_and_count_warnings(fd, a, false);
	unlink(a);
	ASSERT_EQ(0, warns);
	PASS();
}

SUITE(io_suite)
{
	RUN_TEST(io_room_reports_free_space);
	RUN_TEST(io_order_across_wraps);
	RUN_TEST(io_pwrite_after_appends);
	RUN_TEST(io_write_larger_than_ring);
	RUN_TEST(io_error_reported);
	RUN_TEST(io_slow_sync_does_not_block_writes);
	RUN_TEST(io_close_after_its_syncs);
	RUN_TEST(io_syncs_coalesce);
	RUN_TEST(io_sync_error_reported);
	RUN_TEST(io_close_fsync_failure_still_closes);
	RUN_TEST(io_close_clean_is_quiet);
}

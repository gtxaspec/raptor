#include "greatest.h"
#include "rmr_io.h"

#include <errno.h>
#include <fcntl.h>
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

SUITE(io_suite)
{
	RUN_TEST(io_order_across_wraps);
	RUN_TEST(io_pwrite_after_appends);
	RUN_TEST(io_write_larger_than_ring);
	RUN_TEST(io_error_reported);
}

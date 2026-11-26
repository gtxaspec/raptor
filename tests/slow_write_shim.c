/*
 * slow_write_shim.c -- LD_PRELOAD: a slow card
 *
 * Writes to regular files go at SLOW_WRITE_KBPS (KB/s, shared by every
 * thread of the process), and fdatasync/fsync take SLOW_SYNC_MS, so rmr
 * meets the storage an SD card is. Other fds (sockets, pipes, rings,
 * stdout/stderr) are untouched; unset variables leave that part alone.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static double next_free; /* when the "card" is idle again, seconds */

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void pace(int fd, size_t len)
{
	static double kbps = -1;
	if (kbps < 0) {
		const char *e = getenv("SLOW_WRITE_KBPS");
		kbps = e ? atof(e) : 0;
	}
	/* stdout/stderr are the daemon's log, often redirected to a file:
	 * pacing them would stall the main loop on its own log lines */
	struct stat st;
	if (kbps <= 0 || len == 0 || fd <= 2 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
		return;
	pthread_mutex_lock(&lock);
	double t = now_s();
	if (next_free < t)
		next_free = t;
	next_free += (double)len / (kbps * 1024.0);
	double wait = next_free - t;
	pthread_mutex_unlock(&lock);
	if (wait > 0)
		usleep((useconds_t)(wait * 1e6));
}

static size_t iov_len(const struct iovec *iov, int cnt)
{
	size_t n = 0;
	for (int i = 0; i < cnt; i++)
		n += iov[i].iov_len;
	return n;
}

ssize_t write(int fd, const void *buf, size_t len)
{
	static ssize_t (*real)(int, const void *, size_t);
	if (!real)
		real = (ssize_t(*)(int, const void *, size_t))dlsym(RTLD_NEXT, "write");
	pace(fd, len);
	return real(fd, buf, len);
}

ssize_t pwrite(int fd, const void *buf, size_t len, off_t off)
{
	static ssize_t (*real)(int, const void *, size_t, off_t);
	if (!real)
		real = (ssize_t(*)(int, const void *, size_t, off_t))dlsym(RTLD_NEXT, "pwrite");
	pace(fd, len);
	return real(fd, buf, len, off);
}

ssize_t writev(int fd, const struct iovec *iov, int cnt)
{
	static ssize_t (*real)(int, const struct iovec *, int);
	if (!real)
		real = (ssize_t(*)(int, const struct iovec *, int))dlsym(RTLD_NEXT, "writev");
	pace(fd, iov_len(iov, cnt));
	return real(fd, iov, cnt);
}

ssize_t pwritev(int fd, const struct iovec *iov, int cnt, off_t off)
{
	static ssize_t (*real)(int, const struct iovec *, int, off_t);
	if (!real)
		real = (ssize_t(*)(int, const struct iovec *, int, off_t))dlsym(RTLD_NEXT,
										"pwritev");
	pace(fd, iov_len(iov, cnt));
	return real(fd, iov, cnt, off);
}

static void sync_pause(void)
{
	static int ms = -1;
	if (ms < 0) {
		const char *e = getenv("SLOW_SYNC_MS");
		ms = e ? atoi(e) : 0;
	}
	if (ms > 0)
		usleep((useconds_t)ms * 1000);
}

int fdatasync(int fd)
{
	static int (*real)(int);
	if (!real)
		real = (int (*)(int))dlsym(RTLD_NEXT, "fdatasync");
	sync_pause();
	return real(fd);
}

int fsync(int fd)
{
	static int (*real)(int);
	if (!real)
		real = (int (*)(int))dlsym(RTLD_NEXT, "fsync");
	sync_pause();
	return real(fd);
}

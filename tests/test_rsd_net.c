/*
 * test_rsd_net.c -- the event loop's receive never waits
 */

#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "greatest.h"
#include "../rsd/rsd_net.h"

static int udp_pair(int *peer)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
	socklen_t al = sizeof(a);
	if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 ||
	    getsockname(fd, (struct sockaddr *)&a, &al) != 0)
		return -1;
	*peer = socket(AF_INET, SOCK_DGRAM, 0);
	if (*peer < 0 || connect(*peer, (struct sockaddr *)&a, sizeof(a)) != 0)
		return -1;
	/* a blocking recv here would return after a second, not never */
	struct timeval tv = {.tv_sec = 1};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	return fd;
}

static long elapsed_ms(const struct timespec *t0)
{
	struct timespec t1;
	clock_gettime(CLOCK_MONOTONIC, &t1);
	return (t1.tv_sec - t0->tv_sec) * 1000 + (t1.tv_nsec - t0->tv_nsec) / 1000000;
}

/* A blocking socket with nothing queued: what a stale event leaves the loop on */
TEST an_empty_blocking_socket_returns_at_once(void)
{
	int peer, fd = udp_pair(&peer);
	ASSERT(fd >= 0);
	char buf[64];
	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	ssize_t n = rsd_recv_nowait(fd, buf, sizeof(buf));
	int err = errno;
	long ms = elapsed_ms(&t0);
	close(fd);
	close(peer);
	ASSERT_EQ(-1, n);
	ASSERT(err == EAGAIN || err == EWOULDBLOCK);
	ASSERT(ms < 200);
	PASS();
}

TEST queued_data_is_still_read(void)
{
	int peer, fd = udp_pair(&peer);
	ASSERT(fd >= 0);
	ASSERT_EQ(4, send(peer, "rtcp", 4, 0));
	char buf[64];
	ssize_t n = -1;
	for (int i = 0; i < 50 && n < 0; i++) { /* loopback delivery is not instant */
		n = rsd_recv_nowait(fd, buf, sizeof(buf));
		if (n < 0)
			usleep(2000);
	}
	close(fd);
	close(peer);
	ASSERT_EQ(4, n);
	ASSERT_EQ(0, memcmp(buf, "rtcp", 4));
	PASS();
}

SUITE(rsd_net_suite)
{
	RUN_TEST(an_empty_blocking_socket_returns_at_once);
	RUN_TEST(queued_data_is_still_read);
}

/*
 * test_rsd_conn.c -- the event loop takes a connection's write lock without waiting
 */

#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "greatest.h"
#include "../rsd/rsd_conn.h"

static long elapsed_ms(const struct timespec *t0)
{
	struct timespec t1;
	clock_gettime(CLOCK_MONOTONIC, &t1);
	return (t1.tv_sec - t0->tv_sec) * 1000 + (t1.tv_nsec - t0->tv_nsec) / 1000000;
}

static void fill(int fd)
{
	char junk[4096] = {0};
	while (send(fd, junk, sizeof(junk), MSG_DONTWAIT) > 0)
		;
}

static void drain(int fd)
{
	char buf[65536];
	while (recv(fd, buf, sizeof(buf), MSG_DONTWAIT) > 0)
		;
}

static void *hold_lock(void *arg)
{
	pthread_mutex_t *l = arg;
	pthread_mutex_lock(l);
	usleep(600 * 1000);
	pthread_mutex_unlock(l);
	return NULL;
}

TEST an_idle_connection_is_writable(void)
{
	int sv[2];
	ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
	ASSERT(rsd_fd_writable(sv[0]));
	close(sv[0]);
	close(sv[1]);
	PASS();
}

TEST a_full_send_buffer_is_not_writable(void)
{
	int sv[2];
	ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
	fill(sv[0]);
	ASSERT(!rsd_fd_writable(sv[0]));
	drain(sv[1]);
	ASSERT(rsd_fd_writable(sv[0]));
	close(sv[0]);
	close(sv[1]);
	PASS();
}

TEST a_dead_peer_counts_as_writable_because_the_write_fails_at_once(void)
{
	int sv[2];
	ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
	fill(sv[0]);
	close(sv[1]);
	ASSERT(rsd_fd_writable(sv[0]));
	close(sv[0]);
	PASS();
}

TEST a_free_lock_on_a_writable_connection_is_taken(void)
{
	int sv[2];
	pthread_mutex_t l = PTHREAD_MUTEX_INITIALIZER;
	ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
	ASSERT(rsd_control_lock(&l, sv[0]));
	ASSERT_NEQ(0, pthread_mutex_trylock(&l)); /* really held */
	pthread_mutex_unlock(&l);
	close(sv[0]);
	close(sv[1]);
	PASS();
}

TEST a_lock_held_by_the_send_thread_is_refused_without_waiting(void)
{
	int sv[2];
	pthread_mutex_t l = PTHREAD_MUTEX_INITIALIZER;
	pthread_t t;
	ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
	ASSERT_EQ(0, pthread_create(&t, NULL, hold_lock, &l));
	usleep(100 * 1000);
	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	bool got = rsd_control_lock(&l, sv[0]);
	long ms = elapsed_ms(&t0);
	pthread_join(t, NULL);
	close(sv[0]);
	close(sv[1]);
	ASSERT(!got);
	ASSERT(ms < 200);
	PASS();
}

TEST a_full_socket_is_refused_and_leaves_the_lock_free(void)
{
	int sv[2];
	pthread_mutex_t l = PTHREAD_MUTEX_INITIALIZER;
	ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
	fill(sv[0]);
	ASSERT(!rsd_control_lock(&l, sv[0]));
	ASSERT_EQ(0, pthread_mutex_trylock(&l));
	pthread_mutex_unlock(&l);
	close(sv[0]);
	close(sv[1]);
	PASS();
}

SUITE(rsd_conn_suite)
{
	RUN_TEST(an_idle_connection_is_writable);
	RUN_TEST(a_full_send_buffer_is_not_writable);
	RUN_TEST(a_dead_peer_counts_as_writable_because_the_write_fails_at_once);
	RUN_TEST(a_free_lock_on_a_writable_connection_is_taken);
	RUN_TEST(a_lock_held_by_the_send_thread_is_refused_without_waiting);
	RUN_TEST(a_full_socket_is_refused_and_leaves_the_lock_free);
}

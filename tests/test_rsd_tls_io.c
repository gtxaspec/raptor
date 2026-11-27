/*
 * test_rsd_tls_io.c -- a TLS read is attempted only when it cannot wait
 */

#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "greatest.h"
#include "../rsd/rsd_tls_io.h"

static void rec(unsigned char *out, unsigned char type, size_t body)
{
	out[0] = type;
	out[1] = 3;
	out[2] = 3;
	out[3] = (unsigned char)(body >> 8);
	out[4] = (unsigned char)body;
}

static int pair(int sv[2])
{
	return socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
}

TEST header_sizes(void)
{
	unsigned char h[5];
	rec(h, 23, 0x4000);
	ASSERT_EQ(5 + 0x4000, rsd_tls_record_size(h));
	rec(h, 22, 0);
	ASSERT_EQ(5, rsd_tls_record_size(h));
	rec(h, 23, RSD_TLS_MAX_BODY);
	ASSERT_EQ(5 + RSD_TLS_MAX_BODY, rsd_tls_record_size(h));
	PASS();
}

TEST implausible_headers_are_not_records(void)
{
	unsigned char h[5];
	rec(h, 23, RSD_TLS_MAX_BODY + 1);
	ASSERT_EQ(-1, rsd_tls_record_size(h));
	rec(h, 19, 10);
	ASSERT_EQ(-1, rsd_tls_record_size(h));
	rec(h, 25, 10);
	ASSERT_EQ(-1, rsd_tls_record_size(h));
	rec(h, 23, 10);
	h[1] = 2;
	ASSERT_EQ(-1, rsd_tls_record_size(h));
	PASS();
}

TEST nothing_queued_is_not_ready(void)
{
	int sv[2];
	ASSERT_EQ(0, pair(sv));
	ASSERT_EQ(0, rsd_tls_record_ready(sv[0]));
	close(sv[0]);
	close(sv[1]);
	PASS();
}

TEST a_peer_that_closed_is_not_left_waiting(void)
{
	int sv[2];
	ASSERT_EQ(0, pair(sv));
	close(sv[1]);
	ASSERT_EQ(-1, rsd_tls_record_ready(sv[0]));
	close(sv[0]);
	PASS();
}

TEST a_short_header_is_not_ready(void)
{
	int sv[2];
	unsigned char h[5];
	rec(h, 23, 10);
	ASSERT_EQ(0, pair(sv));
	ASSERT_EQ(4, send(sv[1], h, 4, 0));
	ASSERT_EQ(0, rsd_tls_record_ready(sv[0]));
	close(sv[0]);
	close(sv[1]);
	PASS();
}

TEST half_a_record_is_not_ready_until_it_completes(void)
{
	int sv[2];
	unsigned char buf[5 + 100];
	memset(buf, 0, sizeof(buf));
	rec(buf, 23, 100);
	ASSERT_EQ(0, pair(sv));
	ASSERT_EQ(5, send(sv[1], buf, 5, 0)); /* header promising 100 bytes */
	ASSERT_EQ(0, rsd_tls_record_ready(sv[0]));
	ASSERT_EQ(50, send(sv[1], buf + 5, 50, 0));
	ASSERT_EQ(0, rsd_tls_record_ready(sv[0]));
	ASSERT_EQ(50, send(sv[1], buf + 55, 50, 0));
	ASSERT_EQ(1, rsd_tls_record_ready(sv[0]));
	close(sv[0]);
	close(sv[1]);
	PASS();
}

TEST the_peek_leaves_the_bytes_for_the_read(void)
{
	int sv[2];
	unsigned char buf[5 + 8], out[5 + 8];
	memset(buf, 7, sizeof(buf));
	rec(buf, 23, 8);
	ASSERT_EQ(0, pair(sv));
	ASSERT_EQ((ssize_t)sizeof(buf), send(sv[1], buf, sizeof(buf), 0));
	ASSERT_EQ(1, rsd_tls_record_ready(sv[0]));
	ASSERT_EQ(1, rsd_tls_record_ready(sv[0]));
	ASSERT_EQ((ssize_t)sizeof(out), recv(sv[0], out, sizeof(out), 0));
	ASSERT_EQ(0, memcmp(buf, out, sizeof(buf)));
	close(sv[0]);
	close(sv[1]);
	PASS();
}

TEST a_second_record_behind_the_first_does_not_matter(void)
{
	int sv[2];
	unsigned char buf[(5 + 4) + 5];
	memset(buf, 0, sizeof(buf));
	rec(buf, 23, 4);
	rec(buf + 9, 23, 0x4000); /* the next record is still in flight */
	ASSERT_EQ(0, pair(sv));
	ASSERT_EQ((ssize_t)sizeof(buf), send(sv[1], buf, sizeof(buf), 0));
	ASSERT_EQ(1, rsd_tls_record_ready(sv[0]));
	close(sv[0]);
	close(sv[1]);
	PASS();
}

TEST plaintext_garbage_is_flagged(void)
{
	int sv[2];
	ASSERT_EQ(0, pair(sv));
	ASSERT_EQ(8, send(sv[1], "GET / HT", 8, 0));
	ASSERT_EQ(-1, rsd_tls_record_ready(sv[0]));
	close(sv[0]);
	close(sv[1]);
	PASS();
}

SUITE(rsd_tls_io_suite)
{
	RUN_TEST(header_sizes);
	RUN_TEST(implausible_headers_are_not_records);
	RUN_TEST(nothing_queued_is_not_ready);
	RUN_TEST(a_peer_that_closed_is_not_left_waiting);
	RUN_TEST(a_short_header_is_not_ready);
	RUN_TEST(half_a_record_is_not_ready_until_it_completes);
	RUN_TEST(the_peek_leaves_the_bytes_for_the_read);
	RUN_TEST(a_second_record_behind_the_first_does_not_matter);
	RUN_TEST(plaintext_garbage_is_flagged);
}

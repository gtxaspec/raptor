/*
 * test_rhd_slots.c -- who is allowed to keep holding one of the eight slots
 *
 * A camera serves RHD_MAX_CLIENTS connections at once and refuses the ninth
 * with 503. Before this, the only connection the daemon ever took a slot back
 * from on its own was one it was mid-reply to; a connection that opened and
 * then said nothing kept its slot until the daemon restarted. Eight of those
 * cost nothing to make and turned off snapshots, MJPEG and the console.
 *
 * So the question each of these asks is the same one: is this client still
 * owing the server a request, or is it in some state that already carries its
 * own deadline? Only the first kind may be dropped for going quiet. The four
 * tests that say "kept" are the ones that matter -- a timeout that also reaped
 * live streams would be a worse bug than the one it replaced.
 *
 * Time is passed in rather than waited for; nothing here sleeps.
 */
#include <string.h>

#include "greatest.h"

#include "../rhd/rhd.h"

/* A client that connected at t=0 and has done nothing since. */
static rhd_client_t at_rest(void)
{
	rhd_client_t c;
	memset(&c, 0, sizeof(c));
	c.recv_start = 0;
	return c;
}

static const int64_t LATE = RHD_RECV_TIMEOUT_MS + 1;

TEST a_client_that_never_asks_for_anything_is_dropped(void)
{
	rhd_client_t c = at_rest();

	ASSERT(rhd_client_recv_expired(&c, LATE));
	PASS();
}

/*
 * The half-open connection does not have to be silent. Sending a request line
 * and stopping short of the blank line that ends the headers is the cheaper
 * attack, because it looks like a real client to anything watching bytes.
 */
TEST a_half_sent_request_does_not_buy_more_time(void)
{
	rhd_client_t c = at_rest();
	const char *partial = "GET / HTTP/1.1\r\n";

	memcpy(c.recv_buf, partial, strlen(partial));
	c.recv_len = strlen(partial);

	ASSERT(rhd_client_recv_expired(&c, LATE));
	PASS();
}

/*
 * The window is for a real client on a bad link, so it has to be a window and
 * not a race: a request still arriving is not late.
 */
TEST a_client_still_within_the_window_is_kept(void)
{
	rhd_client_t c = at_rest();

	ASSERT_FALSE(rhd_client_recv_expired(&c, RHD_RECV_TIMEOUT_MS / 2));
	ASSERTm("the deadline itself is not yet past",
		!rhd_client_recv_expired(&c, RHD_RECV_TIMEOUT_MS));
	PASS();
}

/*
 * An MJPEG client asks once and then holds the connection open for hours by
 * design. It is the obvious thing to break here.
 */
TEST an_mjpeg_stream_is_kept(void)
{
	rhd_client_t c = at_rest();
	c.is_mjpeg = true;

	ASSERT_FALSE(rhd_client_recv_expired(&c, LATE));
	PASS();
}

TEST an_audio_stream_is_kept(void)
{
	rhd_client_t c = at_rest();
	c.is_audio = true;

	ASSERT_FALSE(rhd_client_recv_expired(&c, LATE));
	PASS();
}

/*
 * A parked /snap is waiting on an idle JPEG encoder, not on the client, and
 * RHD_SNAP_TIMEOUT_MS already bounds it.
 */
TEST a_parked_snapshot_is_kept(void)
{
	rhd_client_t c = at_rest();
	c.snap_pending = true;
	c.snap_deadline = LATE + RHD_SNAP_TIMEOUT_MS;

	ASSERT_FALSE(rhd_client_recv_expired(&c, LATE));
	PASS();
}

/*
 * A response still draining belongs to RHD_SEND_TIMEOUT_MS, which is shorter
 * than this one; reaping it here would take the connection away mid-reply.
 */
TEST a_draining_response_is_kept(void)
{
	rhd_client_t c = at_rest();
	uint8_t body[] = "HTTP/1.1 200 OK\r\n\r\n";

	c.send_buf = body;
	c.send_len = sizeof(body);
	c.send_start = LATE;

	ASSERT_FALSE(rhd_client_recv_expired(&c, LATE));
	PASS();
}

SUITE(rhd_slots_suite)
{
	RUN_TEST(a_client_that_never_asks_for_anything_is_dropped);
	RUN_TEST(a_half_sent_request_does_not_buy_more_time);
	RUN_TEST(a_client_still_within_the_window_is_kept);
	RUN_TEST(an_mjpeg_stream_is_kept);
	RUN_TEST(an_audio_stream_is_kept);
	RUN_TEST(a_parked_snapshot_is_kept);
	RUN_TEST(a_draining_response_is_kept);
}

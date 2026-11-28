/*
 * test_rmd_tracker.c -- rmd's PTZ tracking loop against a fake rvd and a fake head, and
 * rmd_ptz.c's wire format against a fake motors-daemon on a socket
 */

#include "greatest.h"
#include "../rmd/rmd.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

/* ── the loop, with fakes ── */

typedef struct {
	int x0, y0, x1, y1; /* the one box rvd reports; x1 <= x0 for none */
	int64_t ts;	    /* its timestamp; 0 = now */
	int busy;	    /* what the head says while moving */
	int moves, last_dx, last_dy;
	int go_backs;
	int detection_calls;
} fake_t;

static int fake_detections(void *user, char *buf, int size)
{
	fake_t *f = user;
	f->detection_calls++;
	int64_t ts = f->ts ? f->ts : rss_timestamp_us();
	if (f->x1 <= f->x0)
		return snprintf(buf, size,
				"{\"status\":\"ok\",\"count\":0,\"width\":640,\"height\":360,"
				"\"timestamp\":%lld,\"detections\":[]}",
				(long long)ts);
	return snprintf(buf, size,
			"{\"status\":\"ok\",\"count\":1,\"width\":640,\"height\":360,"
			"\"timestamp\":%lld,\"detections\":[{\"x0\":%d,\"y0\":%d,\"x1\":%d,"
			"\"y1\":%d,\"confidence\":0.9,\"class\":0}]}",
			(long long)ts, f->x0, f->y0, f->x1, f->y1);
}

static int fake_busy(void *user)
{
	return ((fake_t *)user)->busy;
}

static int fake_move(void *user, int dx, int dy, int speed)
{
	(void)speed;
	fake_t *f = user;
	f->moves++;
	f->last_dx = dx;
	f->last_dy = dy;
	return 0;
}

static int fake_go_back(void *user)
{
	((fake_t *)user)->go_backs++;
	return 0;
}

static const rmd_track_io_t fake_io = {fake_detections, fake_busy, fake_move, fake_go_back};

/* 620/400 steps a frame, dead zone 15/25 %, gain 80 %, aim at the box centre, no settle,
 * one result a move, motion active. */
static void setup(rmd_ctx_t *ctx, fake_t *f)
{
	memset(ctx, 0, sizeof(*ctx));
	memset(f, 0, sizeof(*f));
	rmd_track_settings_t *t = &ctx->track_cfg;
	t->enabled = true;
	t->aim.min_confidence = 0.3f;
	t->aim.deadzone_pct = 15;
	t->aim.deadzone_y_pct = 25;
	t->aim.aim_y_pct = 50;
	t->aim.steps_x_per_frame = 620;
	t->aim.steps_y_per_frame = 400;
	t->aim.gain_pct = 80;
	t->settle_ms = 0;
	t->average = 1;
	t->lost_sec = 5;
	t->home_on_idle = true;
	ctx->track_io = &fake_io;
	ctx->track_user = f;
	ctx->state = RMD_STATE_ACTIVE;
}

static void box(fake_t *f, int cx, int cy)
{
	f->x0 = cx - 20;
	f->x1 = cx + 20;
	f->y0 = cy - 40;
	f->y1 = cy + 40;
}

/* Move toward the box, wait while the head runs, stop, look again. */
TEST tracker_move_stop_look(void)
{
	rmd_ctx_t ctx;
	fake_t f;
	setup(&ctx, &f);

	box(&f, 120, 180); /* left of centre */
	rmd_track_step(&ctx);
	ASSERT_EQ(1, f.moves);
	ASSERT(f.last_dx < 0);
	ASSERT_EQ(0, f.last_dy);
	ASSERT(ctx.track.moving);

	/* while the head runs, no look and no move */
	f.busy = 1;
	int calls = f.detection_calls;
	rmd_track_step(&ctx);
	rmd_track_step(&ctx);
	ASSERT_EQ(1, f.moves);
	ASSERT_EQ(calls, f.detection_calls);

	/* stopped: the step that sees it only notes the stop */
	f.busy = 0;
	rmd_track_step(&ctx);
	ASSERT_FALSE(ctx.track.moving);
	ASSERT_EQ(1, f.moves);

	/* now centred: no move */
	usleep(2000);
	box(&f, 320, 180);
	rmd_track_step(&ctx);
	ASSERT_EQ(1, f.moves);
	PASS();
}

/* A result read twice counts once: with average = 2, the same result twice is not two. */
TEST tracker_needs_a_new_result(void)
{
	rmd_ctx_t ctx;
	fake_t f;
	setup(&ctx, &f);
	ctx.track_cfg.average = 2;

	f.ts = rss_timestamp_us();
	box(&f, 120, 180);
	rmd_track_step(&ctx);
	rmd_track_step(&ctx);
	ASSERT_EQ(0, f.moves);
	f.ts++;
	rmd_track_step(&ctx);
	ASSERT_EQ(1, f.moves);
	PASS();
}

/* A move only when the tracker has moved the head: motion that ends untracked does not
 * send the head back. */
TEST tracker_no_go_back_unmoved(void)
{
	rmd_ctx_t ctx;
	fake_t f;
	setup(&ctx, &f);

	box(&f, 320, 180); /* centred: seen, no move */
	rmd_track_step(&ctx);
	ASSERT(ctx.track.have_prev);
	ctx.state = RMD_STATE_IDLE;
	rmd_track_step(&ctx);
	ASSERT_EQ(0, f.go_backs);
	PASS();
}

/* A result stamped within settle_ms of the stop may come from a frame taken while moving. */
TEST tracker_settle(void)
{
	rmd_ctx_t ctx;
	fake_t f;
	setup(&ctx, &f);
	ctx.track_cfg.settle_ms = 1000;
	ctx.track.stop_us = rss_timestamp_us();

	box(&f, 120, 180);
	f.ts = ctx.track.stop_us + 500000;
	rmd_track_step(&ctx);
	ASSERT_EQ(0, f.moves);
	f.ts = ctx.track.stop_us + 1000000;
	rmd_track_step(&ctx);
	ASSERT_EQ(1, f.moves);
	PASS();
}

/* average = 2: the move waits for the second result and aims at the mean. */
TEST tracker_average(void)
{
	rmd_ctx_t ctx;
	fake_t f;
	setup(&ctx, &f);
	ctx.track_cfg.average = 2;

	box(&f, 100, 180);
	f.ts = rss_timestamp_us();
	rmd_track_step(&ctx);
	ASSERT_EQ(0, f.moves);
	box(&f, 140, 180);
	f.ts++;
	rmd_track_step(&ctx);
	ASSERT_EQ(1, f.moves);
	/* mean x 120: -0.3125 of the frame, 620 steps, 80 % */
	ASSERT_EQ(-155, f.last_dx);
	PASS();
}

/* Shadow: moves counted and logged, the head left alone. */
TEST tracker_shadow(void)
{
	rmd_ctx_t ctx;
	fake_t f;
	setup(&ctx, &f);
	ctx.track_cfg.shadow = true;

	box(&f, 120, 180);
	rmd_track_step(&ctx);
	ASSERT_EQ(0, f.moves);
	ASSERT_EQ(1, ctx.track.moves);
	ASSERT_FALSE(ctx.track.moving);
	ctx.state = RMD_STATE_IDLE;
	rmd_track_step(&ctx);
	ASSERT_EQ(0, f.go_backs);
	PASS();
}

/* Motion over: back once, only if the tracker moved; and on shutdown. */
TEST tracker_go_back(void)
{
	rmd_ctx_t ctx;
	fake_t f;
	setup(&ctx, &f);

	ctx.state = RMD_STATE_IDLE;
	rmd_track_step(&ctx);
	ASSERT_EQ(0, f.go_backs);

	ctx.state = RMD_STATE_ACTIVE;
	box(&f, 120, 180);
	rmd_track_step(&ctx);
	ASSERT_EQ(1, f.moves);
	ctx.state = RMD_STATE_IDLE;
	rmd_track_step(&ctx);
	rmd_track_step(&ctx);
	ASSERT_EQ(1, f.go_backs);

	ctx.state = RMD_STATE_ACTIVE;
	rmd_track_step(&ctx);
	ASSERT_EQ(2, f.moves);
	rmd_track_shutdown(&ctx);
	ASSERT_EQ(2, f.go_backs);
	PASS();
}

/* A box cut off at the frame's bottom never tilts the head up: aimed high on a tall box cut
 * by the bottom edge (the bench case of a person lying at the bottom of the view), the move
 * up is dropped. */
TEST tracker_edge(void)
{
	rmd_ctx_t ctx;
	fake_t f;
	setup(&ctx, &f);
	ctx.track_cfg.aim.aim_y_pct = 10;

	f.x0 = 300;
	f.x1 = 340;
	f.y0 = 10;
	f.y1 = 359; /* at the bottom edge; the aim point well above the dead zone */
	rmd_track_step(&ctx);
	ASSERT_EQ(0, f.moves);

	f.y1 = 340; /* clear of the edge: the same aim now tilts up */
	usleep(1000);
	rmd_track_step(&ctx);
	ASSERT_EQ(1, f.moves);
	ASSERT(f.last_dy < 0);
	PASS();
}

/* No box: no move, and the target is forgotten after lost_sec. */
TEST tracker_no_box(void)
{
	rmd_ctx_t ctx;
	fake_t f;
	setup(&ctx, &f);

	f.x1 = f.x0; /* none */
	rmd_track_step(&ctx);
	ASSERT_EQ(0, f.moves);
	ctx.track.have_prev = true;
	ctx.track.last_seen_us = rss_timestamp_us() - 6000000;
	usleep(1000);
	rmd_track_step(&ctx);
	ASSERT_FALSE(ctx.track.have_prev);
	PASS();
}

/* ── rmd_ptz.c's wire format, against a fake motors-daemon ── */

typedef struct {
	int lfd;
	unsigned char req[64];
	ssize_t req_len;
	int reply_status;
} fake_md_t;

static void *fake_md_thread(void *arg)
{
	fake_md_t *m = arg;
	int c = accept(m->lfd, NULL, NULL);
	if (c < 0)
		return NULL;
	m->req_len = read(c, m->req, sizeof(m->req));
	if (m->req_len > 0 && m->req[0] == 'b') {
		int msg[7] = {1525, 775, m->reply_status, 900, 3050, 1550, 0};
		(void)!write(c, msg, sizeof(msg));
	}
	close(c);
	return NULL;
}

static int fake_md_start(fake_md_t *m, const char *path, pthread_t *th)
{
	unlink(path);
	m->lfd = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un a = {.sun_family = AF_UNIX};
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
	if (m->lfd < 0 || bind(m->lfd, (struct sockaddr *)&a, sizeof(a)) < 0 ||
	    listen(m->lfd, 1) < 0)
		return -1;
	return pthread_create(th, NULL, fake_md_thread, m);
}

static int req_int(const fake_md_t *m, int off)
{
	int v;
	memcpy(&v, m->req + off, sizeof(v));
	return v;
}

TEST ptz_move_wire(void)
{
	char path[64];
	snprintf(path, sizeof(path), "/tmp/rmd_md_test_%d.sock", (int)getpid());
	fake_md_t m = {0};
	pthread_t th;
	ASSERT_EQ(0, fake_md_start(&m, path, &th));

	ASSERT_EQ(0, rmd_ptz_move(path, -117, 89, 450));
	pthread_join(th, NULL);
	close(m.lfd);
	unlink(path);

	/* struct request: char command, char type, then int x, got_x, y, got_y, speed */
	ASSERT_EQ(24, (int)m.req_len);
	ASSERT_EQ('d', m.req[0]);
	ASSERT_EQ('g', m.req[1]);
	ASSERT_EQ(-117, req_int(&m, 4));
	ASSERT_EQ(1, req_int(&m, 8));
	ASSERT_EQ(89, req_int(&m, 12));
	ASSERT_EQ(1, req_int(&m, 16));
	ASSERT_EQ(450, req_int(&m, 20));
	PASS();
}

TEST ptz_busy_and_go_back_wire(void)
{
	char path[64];
	snprintf(path, sizeof(path), "/tmp/rmd_md_test_%d.sock", (int)getpid());
	fake_md_t m = {0};
	pthread_t th;

	m.reply_status = 1;
	ASSERT_EQ(0, fake_md_start(&m, path, &th));
	ASSERT_EQ(1, rmd_ptz_busy(path));
	pthread_join(th, NULL);
	close(m.lfd);
	ASSERT_EQ('b', m.req[0]);

	memset(&m, 0, sizeof(m));
	ASSERT_EQ(0, fake_md_start(&m, path, &th));
	ASSERT_EQ(0, rmd_ptz_busy(path));
	pthread_join(th, NULL);
	close(m.lfd);

	memset(&m, 0, sizeof(m));
	ASSERT_EQ(0, fake_md_start(&m, path, &th));
	ASSERT_EQ(0, rmd_ptz_go_back(path));
	pthread_join(th, NULL);
	close(m.lfd);
	unlink(path);
	ASSERT_EQ('d', m.req[0]);
	ASSERT_EQ('b', m.req[1]);

	/* no daemon: an error, not a hang */
	ASSERT_EQ(-1, rmd_ptz_busy(path));
	ASSERT_EQ(-1, rmd_ptz_move(path, 10, 0, 0));
	PASS();
}

SUITE(rmd_tracker_suite)
{
	RUN_TEST(tracker_move_stop_look);
	RUN_TEST(tracker_needs_a_new_result);
	RUN_TEST(tracker_no_go_back_unmoved);
	RUN_TEST(tracker_settle);
	RUN_TEST(tracker_average);
	RUN_TEST(tracker_shadow);
	RUN_TEST(tracker_go_back);
	RUN_TEST(tracker_edge);
	RUN_TEST(tracker_no_box);
	RUN_TEST(ptz_move_wire);
	RUN_TEST(ptz_busy_and_go_back_wire);
}

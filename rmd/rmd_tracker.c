/*
 * rmd_tracker.c -- follow a detection with the PTZ head ([track])
 *
 * Move, stop, look: while motion is active, take the newest boxes from rvd once the head
 * has stopped and settled, pick the target, and send one relative move toward it if its
 * aim point is outside the dead zone. When motion ends the head goes back.
 */

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "rmd.h"

#define RMD_TRACK_MAX_BOXES    32 /* rvd sends at most RSS_IVS_MAX_DETECTIONS, 20 */
#define RMD_TRACK_MAX_AVERAGE  32
#define RMD_TRACK_LIMITS_RETRY 5000000 /* us between asks while the head's range is unknown */

static uint32_t parse_classes(const char *s)
{
	uint32_t mask = 0;
	while (s && *s) {
		char *end;
		long v = strtol(s, &end, 10);
		if (end == s)
			break;
		if (v >= 0 && v < 32)
			mask |= 1u << v;
		s = end;
		while (*s == ',' || *s == ' ')
			s++;
	}
	return mask;
}

/* A [track] integer held to lo..hi: a value outside is a typo or a unit mistake, and a
 * large one would send the head far past what a frame needs. */
static int cfg_int(rss_config_t *cfg, const char *key, int def, int lo, int hi)
{
	int v = rss_config_get_int(cfg, "track", key, def);
	if (v >= lo && v <= hi)
		return v;
	int clamped = v < lo ? lo : hi;
	RSS_WARN("track: %s = %d is outside %d..%d, using %d", key, v, lo, hi, clamped);
	return clamped;
}

void rmd_track_load(rmd_ctx_t *ctx)
{
	rss_config_t *cfg = ctx->cfg;
	rmd_track_settings_t *t = &ctx->track_cfg;

	t->enabled = rss_config_get_bool(cfg, "track", "enabled", false);
	t->shadow = rss_config_get_bool(cfg, "track", "shadow", false);
	t->aim.class_mask = parse_classes(rss_config_get_str(cfg, "track", "classes", ""));
	t->aim.min_confidence = cfg_int(cfg, "min_confidence", 40, 0, 100) / 100.0f;
	t->aim.deadzone_pct = cfg_int(cfg, "deadzone_pct", 15, 0, 100);
	t->aim.deadzone_y_pct = cfg_int(cfg, "deadzone_y_pct", t->aim.deadzone_pct, 0, 100);
	t->aim.aim_y_pct = cfg_int(cfg, "aim_y_pct", 50, 0, 100);
	t->aim.steps_x_per_frame = cfg_int(cfg, "steps_x_per_frame", 0, 0, RMD_TRACK_MAX_STEPS);
	t->aim.steps_y_per_frame = cfg_int(cfg, "steps_y_per_frame", 0, 0, RMD_TRACK_MAX_STEPS);
	t->aim.gain_pct = cfg_int(cfg, "gain_pct", 80, 0, 100);
	t->aim.max_steps_x = cfg_int(cfg, "max_steps_x", 0, 0, RMD_TRACK_MAX_STEPS);
	t->aim.max_steps_y = cfg_int(cfg, "max_steps_y", 0, 0, RMD_TRACK_MAX_STEPS);
	t->settle_ms = cfg_int(cfg, "settle_ms", 1000, 0, 60000);
	t->average = cfg_int(cfg, "average", 1, 1, RMD_TRACK_MAX_AVERAGE);
	t->speed = cfg_int(cfg, "speed", 0, 0, INT_MAX);
	t->lost_sec = cfg_int(cfg, "lost_sec", 5, 0, 86400);
	t->home_on_idle = rss_config_get_bool(cfg, "track", "home_on_idle", true);
	t->invert_x = rss_config_get_bool(cfg, "track", "invert_x", false);
	t->invert_y = rss_config_get_bool(cfg, "track", "invert_y", false);
	rss_strlcpy(t->motors_socket, rss_config_get_str(cfg, "track", "motors_socket", "/dev/md"),
		    sizeof(t->motors_socket));

	if (!t->enabled)
		return;
	if (t->aim.steps_x_per_frame <= 0 && t->aim.steps_y_per_frame <= 0)
		RSS_WARN("track: steps_x_per_frame and steps_y_per_frame are 0, the head will "
			 "not move");
	RSS_INFO("track: on%s (dead zone %d/%d%%, gain %d%%, %d/%d steps a frame, settle %d ms, "
		 "average %d)",
		 t->shadow ? ", shadow" : "", t->aim.deadzone_pct, t->aim.deadzone_y_pct,
		 t->aim.gain_pct, t->aim.steps_x_per_frame, t->aim.steps_y_per_frame, t->settle_ms,
		 t->average);
}

/* The real I/O: rvd's control socket and motors-daemon. */
static int real_detections(void *user, char *buf, int size)
{
	(void)user;
	return rss_ctrl_cmd(RSS_RUN_DIR "/rvd.sock", "ivs-detections", buf, size, 1000);
}

static int real_busy(void *user)
{
	return rmd_ptz_busy(((rmd_ctx_t *)user)->track_cfg.motors_socket);
}

static int real_move(void *user, int dx, int dy, int speed)
{
	return rmd_ptz_move(((rmd_ctx_t *)user)->track_cfg.motors_socket, dx, dy, speed);
}

static int real_go_back(void *user)
{
	return rmd_ptz_go_back(((rmd_ctx_t *)user)->track_cfg.motors_socket);
}

static int real_limits(void *user, int *x_max, int *y_max)
{
	return rmd_ptz_limits(((rmd_ctx_t *)user)->track_cfg.motors_socket, x_max, y_max);
}

const rmd_track_io_t rmd_track_io_real = {
	.detections = real_detections,
	.busy = real_busy,
	.move = real_move,
	.go_back = real_go_back,
	.limits = real_limits,
};

static const rmd_track_io_t *io_of(rmd_ctx_t *ctx, void **user)
{
	if (ctx->track_io) {
		*user = ctx->track_user;
		return ctx->track_io;
	}
	*user = ctx;
	return &rmd_track_io_real;
}

static int json_int(cJSON *obj, const char *key, int def)
{
	cJSON *v = cJSON_GetObjectItem(obj, key);
	return cJSON_IsNumber(v) ? v->valueint : def;
}

/* The newest boxes from rvd. Returns their count, or -1 with no answer. */
static int fetch_boxes(rmd_ctx_t *ctx, rmd_box_t *boxes, int max, int64_t *ts)
{
	char resp[4096];
	void *user;
	const rmd_track_io_t *io = io_of(ctx, &user);
	if (io->detections(user, resp, sizeof(resp)) < 0)
		return -1;
	cJSON *root = cJSON_Parse(resp);
	if (!root)
		return -1;

	int n = -1;
	cJSON *jts = cJSON_GetObjectItem(root, "timestamp");
	cJSON *arr = cJSON_GetObjectItem(root, "detections");
	int w = json_int(root, "width", 0);
	int h = json_int(root, "height", 0);
	if (cJSON_IsNumber(jts) && cJSON_IsArray(arr) && w > 0 && h > 0) {
		*ts = (int64_t)jts->valuedouble;
		ctx->track_cfg.aim.frame_w = w;
		ctx->track_cfg.aim.frame_h = h;
		n = 0;
		cJSON *it;
		cJSON_ArrayForEach(it, arr)
		{
			if (n >= max)
				break;
			rmd_box_t *b = &boxes[n++];
			b->x0 = json_int(it, "x0", 0);
			b->y0 = json_int(it, "y0", 0);
			b->x1 = json_int(it, "x1", 0);
			b->y1 = json_int(it, "y1", 0);
			cJSON *c = cJSON_GetObjectItem(it, "confidence");
			b->confidence = cJSON_IsNumber(c) ? (float)c->valuedouble : 0;
			b->class_id = json_int(it, "class", -1);
		}
	}
	cJSON_Delete(root);
	return n;
}

static void go_back(rmd_ctx_t *ctx)
{
	rmd_track_state_t *s = &ctx->track;
	bool returning = false;
	if (s->moved && ctx->track_cfg.home_on_idle) {
		RSS_INFO("track: motion over after %d move(s), going back%s", s->moves,
			 ctx->track_cfg.shadow ? " (shadow)" : "");
		void *user;
		const rmd_track_io_t *io = io_of(ctx, &user);
		if (!ctx->track_cfg.shadow)
			returning = io->go_back(user) == 0;
	}
	/* A trigger right after must not look at frames taken while the head returns:
	 * wait for it to stop, then settle, as after any move. */
	s->moving = returning;
	if (returning)
		s->stop_us = rss_timestamp_us();
	s->moved = false;
	s->have_prev = false;
	s->moves = 0;
	rmd_track_avg_reset(&s->avg);
}

/* The head's range bounds one move; asked once it answers, retried while it does not. */
static void head_limits(rmd_ctx_t *ctx, const rmd_track_io_t *io, void *user, int64_t now)
{
	rmd_track_state_t *s = &ctx->track;
	if (s->x_limit > 0 || s->y_limit > 0 || !io->limits || ctx->track_cfg.shadow ||
	    (s->limits_try_us && now - s->limits_try_us < RMD_TRACK_LIMITS_RETRY))
		return;
	s->limits_try_us = now;
	int x = 0, y = 0;
	if (io->limits(user, &x, &y) == 0) {
		s->x_limit = x > 0 ? x : 0;
		s->y_limit = y > 0 ? y : 0;
	}
}

static int cap_steps(int configured, int limit)
{
	return limit > 0 && (configured <= 0 || configured > limit) ? limit : configured;
}

void rmd_track_step(rmd_ctx_t *ctx)
{
	rmd_track_settings_t *t = &ctx->track_cfg;
	rmd_track_state_t *s = &ctx->track;
	if (!t->enabled)
		return;

	void *user;
	const rmd_track_io_t *io = io_of(ctx, &user);
	int64_t now = rss_timestamp_us();
	if (ctx->state == RMD_STATE_IDLE) {
		if (s->moved || s->have_prev)
			go_back(ctx);
		return;
	}

	/* Look only once the head has stopped. */
	if (s->moving) {
		int busy = t->shadow ? 0 : io->busy(user);
		if (busy == 1)
			return;
		if (busy < 0)
			RSS_WARN("track: motors-daemon gave no status, taking the head as stopped");
		s->moving = false;
		s->stop_us = now;
		rmd_track_avg_reset(&s->avg);
		return;
	}

	rmd_box_t boxes[RMD_TRACK_MAX_BOXES];
	int64_t ts = 0;
	int n = fetch_boxes(ctx, boxes, RMD_TRACK_MAX_BOXES, &ts);
	if (n < 0 || ts <= s->last_ts)
		return; /* nothing new */
	s->last_ts = ts;
	/* A result stamped this soon after a stop may come from a frame taken while moving. */
	if (ts < s->stop_us + (int64_t)t->settle_ms * 1000)
		return;

	int i = rmd_track_pick(boxes, n, &t->aim, s->have_prev, s->prev_x, s->prev_y);
	if (i < 0) {
		if (s->have_prev && now - s->last_seen_us > (int64_t)t->lost_sec * 1000000) {
			RSS_DEBUG("track: target lost");
			s->have_prev = false;
		}
		return;
	}

	float ax, ay;
	rmd_track_aim_point(&boxes[i], &t->aim, &ax, &ay);
	s->last_seen_us = now;
	s->have_prev = true;
	s->prev_x = ax;
	s->prev_y = ay;

	rmd_track_avg_add(&s->avg, ax, ay);
	if (!rmd_track_avg_ready(&s->avg, t->average, &ax, &ay))
		return;
	rmd_track_avg_reset(&s->avg);

	head_limits(ctx, io, user, now);
	rmd_track_cfg_t aim = t->aim;
	aim.max_steps_x = cap_steps(aim.max_steps_x, s->x_limit);
	aim.max_steps_y = cap_steps(aim.max_steps_y, s->y_limit);

	int dx, dy;
	if (!rmd_track_move_to(ax, ay, &aim, &dx, &dy))
		return;
	if (!rmd_track_edge_limit(&boxes[i], &aim, &dx, &dy)) {
		RSS_DEBUG("track: box at the frame edge, no move away from it");
		return;
	}

	/* After the move the target should sit this much nearer the centre. */
	float keep = 1.0f - t->aim.gain_pct / 100.0f;
	if (dx)
		s->prev_x = t->aim.frame_w / 2.0f + (ax - t->aim.frame_w / 2.0f) * keep;
	if (dy)
		s->prev_y = t->aim.frame_h / 2.0f + (ay - t->aim.frame_h / 2.0f) * keep;

	int mx = t->invert_x ? -dx : dx;
	int my = t->invert_y ? -dy : dy;
	RSS_INFO("track: class %d %.2f at (%.0f,%.0f) of %dx%d, move %d,%d%s", boxes[i].class_id,
		 (double)boxes[i].confidence, (double)ax, (double)ay, t->aim.frame_w,
		 t->aim.frame_h, mx, my, t->shadow ? " (shadow)" : "");
	s->moves++;
	s->moved = true;
	if (t->shadow) {
		s->stop_us = now; /* as if it had moved and stopped */
		rmd_track_avg_reset(&s->avg);
		return;
	}
	if (io->move(user, mx, my, t->speed) == 0)
		s->moving = true;
	else
		RSS_WARN("track: move %d,%d not sent", mx, my);
}

void rmd_track_shutdown(rmd_ctx_t *ctx)
{
	if (!ctx->track_cfg.enabled || !ctx->track.moved || !ctx->track_cfg.home_on_idle ||
	    ctx->track_cfg.shadow)
		return;
	void *user;
	const rmd_track_io_t *io = io_of(ctx, &user);
	io->go_back(user);
}

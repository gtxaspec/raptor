/*
 * rmd_track.c -- PTZ tracking: which box to follow and how far to move
 */

#include "rmd_track.h"

static bool eligible(const rmd_box_t *b, const rmd_track_cfg_t *cfg)
{
	if (b->x1 <= b->x0 || b->y1 <= b->y0)
		return false;
	if (b->confidence < cfg->min_confidence)
		return false;
	if (cfg->class_mask == 0)
		return true;
	return b->class_id >= 0 && b->class_id < 32 && (cfg->class_mask & (1u << b->class_id));
}

void rmd_track_aim_point(const rmd_box_t *b, const rmd_track_cfg_t *cfg, float *x, float *y)
{
	*x = (b->x0 + b->x1) / 2.0f;
	*y = b->y0 + (b->y1 - b->y0) * cfg->aim_y_pct / 100.0f;
}

int rmd_track_pick(const rmd_box_t *boxes, int n, const rmd_track_cfg_t *cfg, bool have_prev,
		   float prev_x, float prev_y)
{
	int best = -1;
	float best_key = 0;

	for (int i = 0; i < n; i++) {
		if (!eligible(&boxes[i], cfg))
			continue;
		float key;
		if (have_prev) {
			float x, y;
			rmd_track_aim_point(&boxes[i], cfg, &x, &y);
			key = (x - prev_x) * (x - prev_x) + (y - prev_y) * (y - prev_y);
			if (best < 0 || key < best_key) {
				best = i;
				best_key = key;
			}
		} else {
			key = boxes[i].confidence;
			if (best < 0 || key > best_key) {
				best = i;
				best_key = key;
			}
		}
	}
	return best;
}

/* Steps for one axis: the error as a share of the frame, times the steps for a frame, times
 * the gain, rounded, clamped. 0 inside the dead zone. */
static int axis_steps(float aim, int size, int deadzone_pct, int steps_per_frame, int gain_pct,
		      int max_steps)
{
	if (size <= 0 || steps_per_frame <= 0)
		return 0;
	float err = (aim - size / 2.0f) / size; /* -0.5 .. 0.5 */
	if ((err < 0 ? -err : err) * 100.0f < deadzone_pct)
		return 0;
	float s = err * steps_per_frame * gain_pct / 100.0f;
	int steps = (int)(s < 0 ? s - 0.5f : s + 0.5f);
	if (max_steps > 0) {
		if (steps > max_steps)
			steps = max_steps;
		if (steps < -max_steps)
			steps = -max_steps;
	}
	return steps;
}

bool rmd_track_move(const rmd_box_t *b, const rmd_track_cfg_t *cfg, int *dx, int *dy)
{
	float x, y;
	rmd_track_aim_point(b, cfg, &x, &y);
	return rmd_track_move_to(x, y, cfg, dx, dy);
}

bool rmd_track_move_to(float x, float y, const rmd_track_cfg_t *cfg, int *dx, int *dy)
{
	*dx = axis_steps(x, cfg->frame_w, cfg->deadzone_pct, cfg->steps_x_per_frame, cfg->gain_pct,
			 cfg->max_steps_x);
	*dy = axis_steps(y, cfg->frame_h, cfg->deadzone_y_pct, cfg->steps_y_per_frame,
			 cfg->gain_pct, cfg->max_steps_y);
	return *dx != 0 || *dy != 0;
}

void rmd_track_avg_reset(rmd_track_avg_t *a)
{
	a->n = 0;
	a->sum_x = 0;
	a->sum_y = 0;
}

void rmd_track_avg_add(rmd_track_avg_t *a, float x, float y)
{
	a->n++;
	a->sum_x += x;
	a->sum_y += y;
}

bool rmd_track_avg_ready(const rmd_track_avg_t *a, int need, float *x, float *y)
{
	if (a->n <= 0 || a->n < need)
		return false;
	*x = a->sum_x / a->n;
	*y = a->sum_y / a->n;
	return true;
}

/* Within this many pixels of the frame's edge, a box is taken as cut off by it. */
#define RMD_TRACK_EDGE_PX 2

static int edge_limit_axis(int lo, int hi, int size, int steps)
{
	bool at_lo = lo <= RMD_TRACK_EDGE_PX;
	bool at_hi = hi >= size - 1 - RMD_TRACK_EDGE_PX;
	if (at_lo && at_hi)
		return 0;
	if (at_lo && steps > 0)
		return 0;
	if (at_hi && steps < 0)
		return 0;
	return steps;
}

bool rmd_track_edge_limit(const rmd_box_t *b, const rmd_track_cfg_t *cfg, int *dx, int *dy)
{
	*dx = edge_limit_axis(b->x0, b->x1, cfg->frame_w, *dx);
	*dy = edge_limit_axis(b->y0, b->y1, cfg->frame_h, *dy);
	return *dx != 0 || *dy != 0;
}

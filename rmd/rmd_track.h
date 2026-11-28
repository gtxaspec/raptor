/*
 * rmd_track.h -- PTZ tracking: which box to follow and how far to move
 *
 * Pure functions, no I/O: rmd_track.c, tested in tests/test_rmd_track.c.
 */

#ifndef RMD_TRACK_H
#define RMD_TRACK_H

#include <stdbool.h>
#include <stdint.h>

/* A detection in the inference frame's pixels, as rvd's ivs-detections gives it. */
typedef struct {
	int x0, y0, x1, y1;
	float confidence;
	int class_id;
} rmd_box_t;

/* The largest single move on an axis, in motor steps: the cap where max_steps is 0, and the
 * bound a [track] value is held to. Larger than any head's range. */
#define RMD_TRACK_MAX_STEPS 10000

typedef struct {
	int frame_w, frame_h;  /* the inference frame */
	uint32_t class_mask;   /* bit n follows class n; 0 follows any class */
	float min_confidence;  /* 0..1 */
	int deadzone_pct;      /* no pan while the aim point is this close to the centre, % of
				  the frame width */
	int deadzone_y_pct;    /* no tilt while it is this close, % of the frame height */
	int aim_y_pct;	       /* aim point down the box, % of its height (50 = centre) */
	int steps_x_per_frame; /* pan steps that move the view one frame width */
	int steps_y_per_frame; /* tilt steps that move the view one frame height */
	int gain_pct;	       /* share of the error corrected by one move */
	int max_steps_x;       /* largest move, 0 = RMD_TRACK_MAX_STEPS */
	int max_steps_y;
} rmd_track_cfg_t;

/* The box to follow: with a previous aim point, the eligible box whose aim point is nearest
 * to it; without one, the most confident. -1 when no box is eligible. */
int rmd_track_pick(const rmd_box_t *boxes, int n, const rmd_track_cfg_t *cfg, bool have_prev,
		   float prev_x, float prev_y);

/* The aim point of a box, in frame pixels. */
void rmd_track_aim_point(const rmd_box_t *b, const rmd_track_cfg_t *cfg, float *x, float *y);

/* The move toward a box: steps for each axis, 0 for an axis whose aim point is inside the
 * dead zone. Positive x is toward the frame's right, positive y toward its bottom. Returns
 * true when either axis moves. */
bool rmd_track_move(const rmd_box_t *b, const rmd_track_cfg_t *cfg, int *dx, int *dy);

/* A box cut off by the frame's edge: its centre is not the object's, and moving the head
 * reveals more of the object and moves the centre, which the next move chases back. On each
 * axis, a box touching one edge only moves the head toward that edge (the object goes on past
 * it); touching both, that axis does not move. Applied to a move from rmd_track_move or
 * rmd_track_move_to; returns true when a move is left. */
bool rmd_track_edge_limit(const rmd_box_t *b, const rmd_track_cfg_t *cfg, int *dx, int *dy);

/* The same toward an aim point in frame pixels. */
bool rmd_track_move_to(float x, float y, const rmd_track_cfg_t *cfg, int *dx, int *dy);

/* The mean aim point of the results seen since the head last stopped: one move is made
 * from several results, so a box that jumps from one result to the next moves the head
 * less. */
typedef struct {
	int n;
	float sum_x, sum_y;
} rmd_track_avg_t;

void rmd_track_avg_reset(rmd_track_avg_t *a);
void rmd_track_avg_add(rmd_track_avg_t *a, float x, float y);
/* true, with the mean, once at least `need` points are in */
bool rmd_track_avg_ready(const rmd_track_avg_t *a, int need, float *x, float *y);

#endif /* RMD_TRACK_H */

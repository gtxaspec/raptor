/*
 * test_rmd_track.c -- PTZ tracking in rmd: the box to follow and the move toward it
 */

#include "greatest.h"
#include "../rmd/rmd_track.h"

/* 640x360 frame, 1000 pan steps a frame width, 500 tilt steps a frame height, 15 % dead
 * zone, aim at the box centre, whole error corrected, no clamp, any class. */
static rmd_track_cfg_t cfg(void)
{
	rmd_track_cfg_t c = {
		.frame_w = 640,
		.frame_h = 360,
		.class_mask = 0,
		.min_confidence = 0.3f,
		.deadzone_pct = 15,
		.deadzone_y_pct = 15,
		.aim_y_pct = 50,
		.steps_x_per_frame = 1000,
		.steps_y_per_frame = 500,
		.gain_pct = 100,
		.max_steps_x = 0,
		.max_steps_y = 0,
	};
	return c;
}

/* A 40x40 box centred on (cx, cy). */
static rmd_box_t box_at(int cx, int cy, float conf, int cls)
{
	rmd_box_t b = {cx - 20, cy - 20, cx + 20, cy + 20, conf, cls};
	return b;
}

TEST track_pick_none_eligible(void)
{
	rmd_track_cfg_t c = cfg();
	rmd_box_t b[2] = {box_at(100, 100, 0.2f, 0), {50, 50, 50, 80, 0.9f, 0}};

	ASSERT_EQ(-1, rmd_track_pick(b, 2, &c, false, 0, 0));
	ASSERT_EQ(-1, rmd_track_pick(b, 0, &c, false, 0, 0));
	PASS();
}

TEST track_pick_most_confident_without_previous(void)
{
	rmd_track_cfg_t c = cfg();
	rmd_box_t b[3] = {box_at(100, 100, 0.5f, 0), box_at(500, 200, 0.9f, 0),
			  box_at(300, 100, 0.7f, 0)};

	ASSERT_EQ(1, rmd_track_pick(b, 3, &c, false, 0, 0));
	PASS();
}

TEST track_pick_nearest_to_previous(void)
{
	rmd_track_cfg_t c = cfg();
	rmd_box_t b[3] = {box_at(100, 100, 0.5f, 0), box_at(500, 200, 0.9f, 0),
			  box_at(300, 100, 0.7f, 0)};

	ASSERT_EQ(0, rmd_track_pick(b, 3, &c, true, 120, 110));
	ASSERT_EQ(2, rmd_track_pick(b, 3, &c, true, 330, 90));
	PASS();
}

TEST track_pick_class_mask(void)
{
	rmd_track_cfg_t c = cfg();
	rmd_box_t b[3] = {box_at(100, 100, 0.9f, 2), box_at(500, 200, 0.6f, 0),
			  box_at(300, 100, 0.8f, 40)};

	c.class_mask = 1u << 0;
	ASSERT_EQ(1, rmd_track_pick(b, 3, &c, false, 0, 0));
	c.class_mask = (1u << 0) | (1u << 2);
	ASSERT_EQ(0, rmd_track_pick(b, 3, &c, false, 0, 0));
	c.class_mask = 1u << 5;
	ASSERT_EQ(-1, rmd_track_pick(b, 3, &c, false, 0, 0));
	PASS();
}

TEST track_aim_point_down_the_box(void)
{
	rmd_track_cfg_t c = cfg();
	rmd_box_t b = {100, 100, 200, 300, 0.9f, 0};
	float x, y;

	rmd_track_aim_point(&b, &c, &x, &y);
	ASSERT_EQ_FMT(150.0f, x, "%f");
	ASSERT_EQ_FMT(200.0f, y, "%f");
	c.aim_y_pct = 25;
	rmd_track_aim_point(&b, &c, &x, &y);
	ASSERT_EQ_FMT(150.0f, y, "%f");
	PASS();
}

TEST track_move_none_in_dead_zone(void)
{
	rmd_track_cfg_t c = cfg();
	int dx = 1, dy = 1;

	rmd_box_t centre = box_at(320, 180, 0.9f, 0);
	ASSERT_FALSE(rmd_track_move(&centre, &c, &dx, &dy));
	ASSERT_EQ(0, dx);
	ASSERT_EQ(0, dy);

	/* 14 % right and 14 % down: still inside 15 % */
	rmd_box_t near = box_at(320 + 89, 180 + 50, 0.9f, 0);
	ASSERT_FALSE(rmd_track_move(&near, &c, &dx, &dy));
	PASS();
}

TEST track_move_one_axis_when_the_other_is_inside(void)
{
	rmd_track_cfg_t c = cfg();
	int dx, dy;

	rmd_box_t right = box_at(480, 180, 0.9f, 0); /* 25 % right, centred vertically */
	ASSERT(rmd_track_move(&right, &c, &dx, &dy));
	ASSERT_EQ(250, dx);
	ASSERT_EQ(0, dy);

	rmd_box_t up = box_at(320, 90, 0.9f, 0); /* 25 % up */
	ASSERT(rmd_track_move(&up, &c, &dx, &dy));
	ASSERT_EQ(0, dx);
	ASSERT_EQ(-125, dy);
	PASS();
}

TEST track_move_signs_gain_and_clamp(void)
{
	rmd_track_cfg_t c = cfg();
	int dx, dy;

	rmd_box_t left_down = box_at(160, 270, 0.9f, 0); /* 25 % left, 25 % down */
	ASSERT(rmd_track_move(&left_down, &c, &dx, &dy));
	ASSERT_EQ(-250, dx);
	ASSERT_EQ(125, dy);

	c.gain_pct = 50;
	ASSERT(rmd_track_move(&left_down, &c, &dx, &dy));
	ASSERT_EQ(-125, dx);
	ASSERT_EQ(63, dy); /* 62.5 rounds away from zero */

	c.gain_pct = 100;
	c.max_steps_x = 100;
	c.max_steps_y = 100;
	ASSERT(rmd_track_move(&left_down, &c, &dx, &dy));
	ASSERT_EQ(-100, dx);
	ASSERT_EQ(100, dy);
	PASS();
}

TEST track_move_unconfigured_axis_stays(void)
{
	rmd_track_cfg_t c = cfg();
	int dx, dy;

	c.steps_y_per_frame = 0; /* a pan-only head */
	rmd_box_t corner = box_at(600, 20, 0.9f, 0);
	ASSERT(rmd_track_move(&corner, &c, &dx, &dy));
	ASSERT(dx > 0);
	ASSERT_EQ(0, dy);
	PASS();
}

/* A wider tilt dead zone holds tilt while pan still moves. */
TEST track_move_tilt_dead_zone(void)
{
	rmd_track_cfg_t c = cfg();
	int dx, dy;

	rmd_box_t b = box_at(480, 270, 0.9f, 0); /* 25 % right, 25 % down */
	ASSERT(rmd_track_move(&b, &c, &dx, &dy));
	ASSERT_EQ(250, dx);
	ASSERT_EQ(125, dy);

	c.deadzone_y_pct = 30;
	ASSERT(rmd_track_move(&b, &c, &dx, &dy));
	ASSERT_EQ(250, dx);
	ASSERT_EQ(0, dy);
	PASS();
}

TEST track_move_to_matches_move(void)
{
	rmd_track_cfg_t c = cfg();
	int dx1, dy1, dx2, dy2;
	rmd_box_t b = box_at(100, 300, 0.9f, 0);
	float x, y;

	rmd_track_aim_point(&b, &c, &x, &y);
	ASSERT_EQ(rmd_track_move(&b, &c, &dx1, &dy1), rmd_track_move_to(x, y, &c, &dx2, &dy2));
	ASSERT_EQ(dx1, dx2);
	ASSERT_EQ(dy1, dy2);
	PASS();
}

TEST track_average(void)
{
	rmd_track_avg_t a;
	float x, y;

	rmd_track_avg_reset(&a);
	ASSERT_FALSE(rmd_track_avg_ready(&a, 1, &x, &y));
	rmd_track_avg_add(&a, 100, 200);
	ASSERT(rmd_track_avg_ready(&a, 1, &x, &y));
	ASSERT_FALSE(rmd_track_avg_ready(&a, 3, &x, &y));
	rmd_track_avg_add(&a, 130, 230);
	rmd_track_avg_add(&a, 70, 140);
	ASSERT(rmd_track_avg_ready(&a, 3, &x, &y));
	ASSERT_EQ_FMT(100.0f, x, "%f");
	ASSERT_EQ_FMT(190.0f, y, "%f");
	rmd_track_avg_reset(&a);
	ASSERT_FALSE(rmd_track_avg_ready(&a, 1, &x, &y));
	PASS();
}

/* Cut off at the left: a move left (toward the cut) stays, a move right goes. */
TEST track_edge_left(void)
{
	rmd_track_cfg_t c = cfg();
	rmd_box_t b = {0, 100, 120, 200, 0.9f, 0};
	int dx = -150, dy = 40;

	ASSERT(rmd_track_edge_limit(&b, &c, &dx, &dy));
	ASSERT_EQ(-150, dx);
	ASSERT_EQ(40, dy);
	dx = 150;
	dy = 0;
	ASSERT_FALSE(rmd_track_edge_limit(&b, &c, &dx, &dy));
	ASSERT_EQ(0, dx);
	PASS();
}

/* Cut off at the bottom: tilt only down; pan untouched. */
TEST track_edge_bottom(void)
{
	rmd_track_cfg_t c = cfg();
	rmd_box_t b = {91, 246, 249, 359, 0.9f, 0}; /* the bench case, 640x360 */
	int dx = -60, dy = -30;

	ASSERT(rmd_track_edge_limit(&b, &c, &dx, &dy));
	ASSERT_EQ(-60, dx);
	ASSERT_EQ(0, dy);
	dy = 30;
	ASSERT(rmd_track_edge_limit(&b, &c, &dx, &dy));
	ASSERT_EQ(30, dy);
	PASS();
}

/* Wider than the frame: no pan at all. */
TEST track_edge_both_sides(void)
{
	rmd_track_cfg_t c = cfg();
	rmd_box_t b = {1, 50, 639, 300, 0.9f, 0};
	int dx = 200, dy = 0;

	ASSERT_FALSE(rmd_track_edge_limit(&b, &c, &dx, &dy));
	dx = -200;
	ASSERT_FALSE(rmd_track_edge_limit(&b, &c, &dx, &dy));
	PASS();
}

/* Clear of the edges by more than 2 px: nothing changes. */
TEST track_edge_clear(void)
{
	rmd_track_cfg_t c = cfg();
	rmd_box_t b = {3, 3, 636, 356, 0.9f, 0};
	int dx = 120, dy = -80;

	ASSERT(rmd_track_edge_limit(&b, &c, &dx, &dy));
	ASSERT_EQ(120, dx);
	ASSERT_EQ(-80, dy);
	PASS();
}

SUITE(rmd_track_suite)
{
	RUN_TEST(track_pick_none_eligible);
	RUN_TEST(track_pick_most_confident_without_previous);
	RUN_TEST(track_pick_nearest_to_previous);
	RUN_TEST(track_pick_class_mask);
	RUN_TEST(track_aim_point_down_the_box);
	RUN_TEST(track_move_none_in_dead_zone);
	RUN_TEST(track_move_one_axis_when_the_other_is_inside);
	RUN_TEST(track_move_signs_gain_and_clamp);
	RUN_TEST(track_move_unconfigured_axis_stays);
	RUN_TEST(track_move_tilt_dead_zone);
	RUN_TEST(track_move_to_matches_move);
	RUN_TEST(track_average);
	RUN_TEST(track_edge_left);
	RUN_TEST(track_edge_bottom);
	RUN_TEST(track_edge_both_sides);
	RUN_TEST(track_edge_clear);
}

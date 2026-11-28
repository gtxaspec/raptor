#include "greatest.h"
#include "rvd_osd_pos.h"

static const char *const named[] = {
	"top_left",	"top_center",  "top_right",    "bottom_left", "bottom_center",
	"bottom_right", "left_center", "right_center", "center",      "nowhere",
};

#define N_NAMED (int)(sizeof(named) / sizeof(named[0]))

TEST a_full_frame_overlay_sits_at_the_origin(void)
{
	/* ROD's detection overlay: 640x360 on a 640x360 sub stream, "0,0".
	 * Held to the margin it would start at (10,10) and end 10 pixels
	 * past the right and bottom edges. */
	int x = -1, y = -1;

	rvd_osd_calc_position(640, 360, 640, 360, "0,0", &x, &y);
	ASSERT_EQ(0, x);
	ASSERT_EQ(0, y);
	PASS();
}

TEST a_region_that_fits_keeps_the_margin(void)
{
	int x = -1, y = -1;

	rvd_osd_calc_position(1920, 1080, 200, 36, "top_left", &x, &y);
	ASSERT_EQ(OSD_MARGIN, x);
	ASSERT_EQ(OSD_MARGIN, y);
	rvd_osd_calc_position(1920, 1080, 200, 36, "bottom_right", &x, &y);
	ASSERT_EQ(1920 - 200 - OSD_MARGIN, x);
	ASSERT_EQ(1080 - 36 - OSD_MARGIN, y);
	rvd_osd_calc_position(1920, 1080, 200, 36, "-50,5000", &x, &y);
	ASSERT_EQ(OSD_MARGIN, x);
	ASSERT_EQ(1080 - 36 - OSD_MARGIN, y);
	PASS();
}

TEST the_margin_is_dropped_only_on_the_axis_that_cannot_hold_it(void)
{
	/* Full height, a third of the width: x keeps its margin. */
	int x = -1, y = -1;

	rvd_osd_calc_position(640, 360, 200, 360, "bottom_right", &x, &y);
	ASSERT_EQ(640 - 200 - OSD_MARGIN, x);
	ASSERT_EQ(0, y);
	PASS();
}

TEST every_region_that_fits_the_frame_lies_inside_it(void)
{
	/* Every named position, every size up to the frame on both axes,
	 * including the widths within 2 * OSD_MARGIN of the frame's. */
	static const int sizes[] = {2, 36, 100, 619, 620, 621, 630, 638, 639, 640};
	const int sw = 640, sh = 360;

	for (int p = 0; p < N_NAMED; p++) {
		for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
			for (unsigned j = 0; j < sizeof(sizes) / sizeof(sizes[0]); j++) {
				int rw = sizes[i], rh = sizes[j] * sh / sw;
				int x = -1, y = -1;

				rvd_osd_calc_position(sw, sh, rw, rh, named[p], &x, &y);
				ASSERT_GTEm(named[p], x, 0);
				ASSERT_GTEm(named[p], y, 0);
				ASSERT_LTEm(named[p], x + rw, sw);
				ASSERT_LTEm(named[p], y + rh, sh);
				ASSERT_EQm("x is even", 0, x & 1);
				ASSERT_EQm("y is even", 0, y & 1);
			}
		}
	}
	PASS();
}

SUITE(osd_pos_suite)
{
	RUN_TEST(a_full_frame_overlay_sits_at_the_origin);
	RUN_TEST(a_region_that_fits_keeps_the_margin);
	RUN_TEST(the_margin_is_dropped_only_on_the_axis_that_cannot_hold_it);
	RUN_TEST(every_region_that_fits_the_frame_lies_inside_it);
}

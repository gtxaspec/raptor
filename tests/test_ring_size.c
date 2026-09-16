#include "greatest.h"
#include "rvd_ring_size.h"

/*
 * Frames measured off two cameras at quality 75. They sit at opposite
 * ends of what the encoder produces per pixel, which is what makes a
 * single average estimate unable to serve both.
 */
#define IMX335_MAIN_W	  2560
#define IMX335_MAIN_H	  1920
#define IMX335_MAIN_FRAME 1214713 /* 0.247 bytes/pixel */

#define IMX335_SUB_W	  640
#define IMX335_SUB_H	  480
#define IMX335_SUB_FRAME  94848 /* 0.309 bytes/pixel */

#define SSC333_MAIN_W	  2304
#define SSC333_MAIN_H	  1296
#define SSC333_MAIN_FRAME 204120 /* 0.068 bytes/pixel */

#define SSC333_SUB_W	  640
#define SSC333_SUB_H	  360
#define SSC333_SUB_FRAME  23397 /* 0.102 bytes/pixel */

TEST a_ring_holds_a_whole_frame_from_either_camera(void)
{
	/* A pool smaller than one frame cannot ever publish: the ring
	 * rejects an oversized frame rather than waiting for room. */
	ASSERT_GTm("imx335 main", rvd_jpeg_ring_data(IMX335_MAIN_W, IMX335_MAIN_H, 75, 1),
		   IMX335_MAIN_FRAME);
	ASSERT_GTm("imx335 sub", rvd_jpeg_ring_data(IMX335_SUB_W, IMX335_SUB_H, 75, 1),
		   IMX335_SUB_FRAME);
	ASSERT_GTm("ssc333 main", rvd_jpeg_ring_data(SSC333_MAIN_W, SSC333_MAIN_H, 75, 1),
		   SSC333_MAIN_FRAME);
	ASSERT_GTm("ssc333 sub", rvd_jpeg_ring_data(SSC333_SUB_W, SSC333_SUB_H, 75, 1),
		   SSC333_SUB_FRAME);
	PASS();
}

TEST the_one_frame_floor_outranks_the_cap(void)
{
	/* The cap exists to bound a pool of many frames. A sensor whose
	 * single frame is bigger than the cap must still get a ring it
	 * can publish into. */
	uint32_t w = 4096, h = 3072;
	uint32_t data = rvd_jpeg_ring_data(w, h, 75, 1);

	ASSERT_GTm("floor was applied over the cap", data, (uint32_t)RVD_JPEG_RING_MAX);
	ASSERT_EQ(rvd_jpeg_frame_bound(w, h), data);
	PASS();
}

TEST a_quiet_camera_is_not_made_to_pay_for_a_busy_one(void)
{
	/* The floor is a floor. Where the slot estimate already clears
	 * one frame -- a high frame rate earns more slots -- the ring
	 * keeps the size the estimate asked for. */
	uint32_t sixteen_slots = rvd_jpeg_ring_data(SSC333_MAIN_W, SSC333_MAIN_H, 75, 10);
	uint32_t estimate = (SSC333_MAIN_W * SSC333_MAIN_H / 16) * 16;

	ASSERT_EQm("16 slots at fps 10", 16, rvd_jpeg_ring_slots(10));
	ASSERT_EQm("estimate stands on its own", estimate, sixteen_slots);
	ASSERT_GTm("and it clears the floor", sixteen_slots,
		   rvd_jpeg_frame_bound(SSC333_MAIN_W, SSC333_MAIN_H));
	PASS();
}

TEST slots_stay_a_power_of_two(void)
{
	/* rss_ring_create refuses anything else, and answers by
	 * returning NULL -- which reaches the caller as a ring that
	 * could not be made, naming nothing about why. */
	uint32_t fps[] = {0, 1, 2, 3, 5, 8, 9, 15, 25, 30, 60};

	for (size_t i = 0; i < sizeof(fps) / sizeof(fps[0]); i++) {
		uint32_t s = rvd_jpeg_ring_slots(fps[i]);
		ASSERT_EQm("power of two", 0u, s & (s - 1));
		ASSERT_GTm("at least one slot", s, 0u);
	}
	PASS();
}

TEST a_small_picture_is_bounded_by_more_than_its_pixels(void)
{
	/* Headers do not shrink with the picture. A bound that is
	 * pixels alone goes to nothing as the stream gets smaller. */
	ASSERT_GTm("160x120 clears a JPEG's fixed cost", rvd_jpeg_frame_bound(160, 120), 32768u);
	PASS();
}

SUITE(ring_size_suite)
{
	RUN_TEST(a_ring_holds_a_whole_frame_from_either_camera);
	RUN_TEST(the_one_frame_floor_outranks_the_cap);
	RUN_TEST(a_quiet_camera_is_not_made_to_pay_for_a_busy_one);
	RUN_TEST(slots_stay_a_power_of_two);
	RUN_TEST(a_small_picture_is_bounded_by_more_than_its_pixels);
}

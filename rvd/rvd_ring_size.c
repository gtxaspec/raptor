/*
 * rvd_ring_size.c -- how big a JPEG ring has to be.
 *
 * Kept apart from the pipeline so the arithmetic can be exercised
 * without a HAL: it is a heuristic over an encoder nobody can predict,
 * and the failure it produces is silent.
 */

#include "rvd_ring_size.h"

uint32_t rvd_jpeg_ring_slots(uint32_t fps)
{
	return (fps >= 9) ? 16 : (fps >= 3) ? 8 : 4;
}

uint32_t rvd_jpeg_frame_bound(uint32_t w, uint32_t h)
{
	/*
	 * An upper bound on one encoded frame, not an average.
	 *
	 * Measured bytes per pixel at quality 75 spans 0.068 on a quiet
	 * 2304x1296 scene to 0.309 on a detailed 640x480 downscale of a
	 * 5MP sensor -- the same picture costs proportionally more the
	 * smaller it is scaled, because the headers do not shrink with
	 * it and neither does the noise. Half a byte per pixel covers
	 * the worst of that with room over, and the fixed term keeps a
	 * small picture from being bounded by its pixel count alone.
	 */
	return w * h / 2 + 32768;
}

uint32_t rvd_jpeg_ring_data(uint32_t w, uint32_t h, uint32_t quality, uint32_t fps)
{
	uint32_t divisor = (quality >= 90) ? 6 : (quality >= 70) ? 16 : 24;
	uint32_t jpeg_avg = w * h / divisor;
	uint32_t bound = rvd_jpeg_frame_bound(w, h);
	uint32_t data;

	if (jpeg_avg < 16384)
		jpeg_avg = 16384;

	data = jpeg_avg * rvd_jpeg_ring_slots(fps);
	if (data > RVD_JPEG_RING_MAX)
		data = RVD_JPEG_RING_MAX;

	/*
	 * The pool has to hold one whole frame. rss_ring_publish refuses
	 * a frame larger than the data region outright, so a pool under
	 * that size is not backpressure that clears on the next frame
	 * but a channel that can never publish again -- the snapshot
	 * endpoint then waits for a frame that will not come. The
	 * estimate above is an average and runs several times under the
	 * bound on a detailed scene, so raise the pool to fit rather
	 * than trusting the slot count to cover the gap. This wins over
	 * the cap: a ring too small to carry its own frames is worth
	 * less than the memory it saves.
	 */
	if (data < bound)
		data = bound;

	return data;
}

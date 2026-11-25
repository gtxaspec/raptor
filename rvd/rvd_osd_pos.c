/*
 * rvd_osd_pos.c -- where an OSD region goes on a stream.
 *
 * Kept apart from rvd_osd.c so the placement can be exercised without a
 * HAL.
 */

#include <stdio.h>
#include <string.h>

#include "rvd_osd_pos.h"

void rvd_osd_calc_position(int stream_w, int stream_h, int region_w, int region_h,
			   const char *pos_str, int *out_x, int *out_y)
{
	int margin_x, margin_y;

	if (pos_str) {
		int x, y;
		if (sscanf(pos_str, "%d,%d", &x, &y) == 2) {
			*out_x = x;
			*out_y = y;
			goto clamp;
		}
	}

	const char *p = pos_str ? pos_str : "top_left";

	if (strcmp(p, "top_left") == 0) {
		*out_x = OSD_MARGIN;
		*out_y = OSD_MARGIN;
	} else if (strcmp(p, "top_center") == 0) {
		*out_x = (stream_w - region_w) / 2;
		*out_y = OSD_MARGIN;
	} else if (strcmp(p, "top_right") == 0) {
		*out_x = stream_w - region_w - OSD_MARGIN;
		*out_y = OSD_MARGIN;
	} else if (strcmp(p, "bottom_left") == 0) {
		*out_x = OSD_MARGIN;
		*out_y = stream_h - region_h - OSD_MARGIN;
	} else if (strcmp(p, "bottom_center") == 0) {
		*out_x = (stream_w - region_w) / 2;
		*out_y = stream_h - region_h - OSD_MARGIN;
	} else if (strcmp(p, "bottom_right") == 0) {
		*out_x = stream_w - region_w - OSD_MARGIN;
		*out_y = stream_h - region_h - OSD_MARGIN;
	} else if (strcmp(p, "left_center") == 0) {
		*out_x = OSD_MARGIN;
		*out_y = (stream_h - region_h) / 2;
	} else if (strcmp(p, "right_center") == 0) {
		*out_x = stream_w - region_w - OSD_MARGIN;
		*out_y = (stream_h - region_h) / 2;
	} else if (strcmp(p, "center") == 0) {
		*out_x = (stream_w - region_w) / 2;
		*out_y = (stream_h - region_h) / 2;
	} else {
		*out_x = OSD_MARGIN;
		*out_y = OSD_MARGIN;
	}

clamp:
	/* A region too large for the margin on an axis gets none on it, so
	 * that it still lies inside the frame. */
	margin_x = region_w + 2 * OSD_MARGIN <= stream_w ? OSD_MARGIN : 0;
	margin_y = region_h + 2 * OSD_MARGIN <= stream_h ? OSD_MARGIN : 0;

	if (*out_x + region_w > stream_w - margin_x)
		*out_x = stream_w - region_w - margin_x;
	if (*out_y + region_h > stream_h - margin_y)
		*out_y = stream_h - region_h - margin_y;
	if (*out_x < margin_x)
		*out_x = margin_x;
	if (*out_y < margin_y)
		*out_y = margin_y;
	*out_x &= ~1;
	*out_y &= ~1;
}

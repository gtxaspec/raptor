#ifndef RVD_OSD_POS_H
#define RVD_OSD_POS_H

/* Pixels kept between a region and the frame's edges, where it fits. */
#define OSD_MARGIN 10

/* Position of a region on a stream from "x,y" or a named position
 * ("top_left", "center", ...), kept inside the frame; even coordinates. */
void rvd_osd_calc_position(int stream_w, int stream_h, int region_w, int region_h,
			   const char *pos_str, int *out_x, int *out_y);

#endif /* RVD_OSD_POS_H */

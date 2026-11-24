#ifndef RVD_FRAME_RESULT_H
#define RVD_FRAME_RESULT_H

/* What the frame loop does with an enc_get_frame() result. */
typedef enum {
	RVD_FRAME_READY,   /* a frame to publish */
	RVD_FRAME_NONE,	   /* nothing this time; poll again */
	RVD_FRAME_STALLED, /* nothing, sustained: warn, then poll again */
	RVD_FRAME_FAILED,  /* the HAL reported an error */
} rvd_frame_result_t;

/* Number of consecutive empty deliveries that counts as a stall. */
#define RVD_FRAME_STALL_POLLS 10

/* Classify ret; *empty_polls carries the run of empty deliveries
 * between calls and starts at 0. STALLED is returned once per run. */
rvd_frame_result_t rvd_frame_result(int ret, int *empty_polls);

#endif /* RVD_FRAME_RESULT_H */

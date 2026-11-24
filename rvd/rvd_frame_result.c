#include <errno.h>

#include "raptor_hal.h"
#include "rvd_frame_result.h"

rvd_frame_result_t rvd_frame_result(int ret, int *empty_polls)
{
	if (ret == -EAGAIN) {
		if (++*empty_polls == RVD_FRAME_STALL_POLLS)
			return RVD_FRAME_STALLED;
		return RVD_FRAME_NONE;
	}
	*empty_polls = 0;
	if (ret == RSS_ERR_SKIPPED)
		return RVD_FRAME_NONE; /* produced, so not stalled */
	return ret == RSS_OK ? RVD_FRAME_READY : RVD_FRAME_FAILED;
}

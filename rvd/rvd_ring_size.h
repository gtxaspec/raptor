#ifndef RVD_RING_SIZE_H
#define RVD_RING_SIZE_H

#include <stdint.h>

/* Ceiling on a JPEG ring's data region, before the one-frame floor. */
#define RVD_JPEG_RING_MAX (4 * 1024 * 1024)

/* Slot count for a JPEG ring at this frame rate; always a power of two. */
uint32_t rvd_jpeg_ring_slots(uint32_t fps);

/* Upper bound on a single encoded JPEG of these dimensions. */
uint32_t rvd_jpeg_frame_bound(uint32_t w, uint32_t h);

/* Size of a JPEG ring's data region: the slot estimate, capped, then
 * raised to hold one whole frame. */
uint32_t rvd_jpeg_ring_data(uint32_t w, uint32_t h, uint32_t quality, uint32_t fps);

#endif /* RVD_RING_SIZE_H */

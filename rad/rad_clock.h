/*
 * rad_clock.h -- synthetic audio capture clock
 *
 * Advances by sample count and slews gently toward CLOCK_MONOTONIC so
 * ADC crystal error and SDK sample loss cannot accumulate as unbounded
 * A/V drift. Extracted from the AI loop so the control law is testable
 * against the failure modes that shaped it.
 */

#ifndef RAD_CLOCK_H
#define RAD_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	int64_t ts; /* next chunk's timestamp */
	int64_t last_read_us;
	int64_t err_ewma_us; /* filtered clock error driving the slew */
	int64_t resync_us;   /* set to the jump size when a stamp hard-resyncs */
} rad_clock_t;

void rad_clock_init(rad_clock_t *c, int64_t now_us);

/*
 * Stamp one chunk of `samples` at `rate`: returns the chunk's timestamp
 * and advances the clock. `now_us` is CLOCK_MONOTONIC at read return.
 * After the call, c->resync_us is nonzero when a hard resync fired (the
 * caller warns; the value is the correction applied).
 */
int64_t rad_clock_stamp(rad_clock_t *c, int samples, int rate, int64_t now_us);

#endif /* RAD_CLOCK_H */

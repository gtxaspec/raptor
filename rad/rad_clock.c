/*
 * rad_clock.c -- synthetic audio capture clock
 *
 * The clock advances by sample count; left alone it drifts with the
 * ADC crystal and silently absorbs samples the SDK loses, so each
 * chunk it slews toward CLOCK_MONOTONIC.
 *
 * The slew is proportional on a filtered error, not a stepped nudge
 * with a deadband. The old deadband let drift wander to its edge
 * (~20ms) and then walked it back in 1ms steps -- a slow sawtooth on
 * the published timeline that every consumer, and every RTCP sender
 * report built from it, faithfully reproduced (measured on a T31:
 * ~2.5ms of mapping motion per 5s SR interval, snapping back tens of
 * ms at the band edge). Filtering the error absorbs chunk-arrival
 * quantization (the reason the deadband existed: an unfiltered 1ms
 * nudge bang-banged at 16kHz where arrival jitter exceeds the nudge),
 * and the proportional term scales corrections down as the error
 * shrinks, so steady state sits within a few hundred microseconds of
 * true rate with no oscillation and no sawtooth.
 *
 * Hard resyncs keep their asymmetric thresholds. Clock BEHIND wall =
 * samples lost or a stall; it never self-heals, so resync at 150ms.
 * Clock AHEAD of wall happens legitimately while draining the SDK's
 * buffered chunks after a stall or at startup (frame_depth ~400ms
 * read back-to-back outruns wall) -- a symmetric threshold fires
 * repeated backward resyncs mid-drain, rewinding the published
 * timeline. Tolerate up to 1s ahead (T23 measured ~760ms of real
 * salvage; decays via the slew in seconds); beyond that something is
 * truly wrong.
 *
 * Resyncs are gated to live-paced reads: an instant return served a
 * chunk the SDK had already buffered (drain burst or scheduler batch)
 * and a long-gap return is the oldest buffered chunk right after a
 * stall -- old audio on the old continuous timeline, where wall
 * comparison misfires. Drain chunks can trickle as slowly as ~15ms on
 * a loaded SoC, so live means close to the 20ms chunk cadence, not
 * merely non-instant. At the first live-paced read after a stall the
 * residual error is exactly the audio the SDK really lost, so the
 * resync inserts a gap of the right size. The slew is NOT gated:
 * during drains it is bounded zero-mean noise, but gating it biases
 * which chunks get evaluated and skews the long-run rate (measured
 * -1000ppm under a +5000ppm test clock; ungated tracks true rate).
 */

#include "rad_clock.h"

#define RAD_SYNTH_RESYNC_BEHIND_US 150000
#define RAD_SYNTH_RESYNC_AHEAD_US  1000000
#define RAD_SYNTH_SLEW_MAX_US	   1000
#define RAD_SYNTH_EWMA_DIV	   16 /* error filter: alpha = 1/16 */
#define RAD_SYNTH_GAIN_DIV	   16 /* correction = filtered error / 16 */

void rad_clock_init(rad_clock_t *c, int64_t now_us)
{
	c->ts = now_us;
	c->last_read_us = now_us;
	c->err_ewma_us = 0;
	c->resync_us = 0;
}

int64_t rad_clock_stamp(rad_clock_t *c, int samples, int rate, int64_t now_us)
{
	int64_t out = c->ts;

	c->resync_us = 0;
	c->ts += (int64_t)samples * 1000000 / rate;

	int64_t read_gap = now_us - c->last_read_us;
	c->last_read_us = now_us;
	bool live_paced = read_gap >= 15000 && read_gap <= 150000;

	int64_t clk_err = now_us - c->ts;
	if (clk_err > RAD_SYNTH_RESYNC_BEHIND_US || clk_err < -RAD_SYNTH_RESYNC_AHEAD_US) {
		if (live_paced) {
			c->ts += clk_err;
			c->err_ewma_us = 0;
			c->resync_us = clk_err;
		}
		return out;
	}

	c->err_ewma_us += (clk_err - c->err_ewma_us) / RAD_SYNTH_EWMA_DIV;
	int64_t slew = c->err_ewma_us / RAD_SYNTH_GAIN_DIV;
	if (slew > RAD_SYNTH_SLEW_MAX_US)
		slew = RAD_SYNTH_SLEW_MAX_US;
	else if (slew < -RAD_SYNTH_SLEW_MAX_US)
		slew = -RAD_SYNTH_SLEW_MAX_US;
	c->ts += slew;

	return out;
}

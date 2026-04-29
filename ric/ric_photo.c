/*
 * ric_photo.c -- Multi-stage EV + AWB day/night detection
 *
 * Uses ISP EV value and white balance R/B gain statistics through
 * a multi-phase state machine with anti-interference filtering.
 *
 * EV direction: on Ingenic, ev = integration_time × gain product.
 * HIGH ev = dark (ISP compensating), LOW ev = bright.
 */

#include "ric.h"

/* EV ratio constants for day/interference detection */
#define DAY_RATIO_LOW	  0.87
#define DAY_RATIO_HIGH	  1.13
#define FIXED_RATIO_LOW	  0.92
#define FIXED_RATIO_HIGH  1.08
#define FIXED_DRIFT_THR	  0.70
#define INTERF_RATIO_HIGH 1.12
#define INTERF_RATIO_LOW  0.88

/* Night detection: R/B gain deviation thresholds (abs difference) */
#define RGAIN_DEV_THR1 15
#define BGAIN_DEV_THR1 10
#define RGAIN_DEV_THR2 9
#define BGAIN_DEV_THR2 7

/* Counter thresholds for night trigger */
#define NIGHT_EV_TRIGGER    23
#define NIGHT_RGAIN_TRIGGER 3
#define NIGHT_BGAIN_TRIGGER 7

/* Interference limits */
#define INTERF_MAX_POLLS    9000
#define INTERF_RISE_TRIGGER 3
#define INTERF_FALL_TRIGGER 3

/* Fixed-EV drift */
#define FIXED_DRIFT_TRIGGER	10
#define FIXED_CHECK_INTERVAL	200
#define FIXED_CHECK_DRIFT_LIMIT 200

/* Anti-flap */
#define ANTI_FLAP_SETTLE 150

void ric_photo_reset(ric_photo_state_t *ps, ric_photo_phase_t phase)
{
	bool was_calibrated = ps->calibrated;
	uint16_t rg_base = ps->rgain_base;
	uint16_t bg_base = ps->bgain_base;
	uint8_t cal_count = ps->cal_count;
	uint32_t rg_sum = ps->cal_rgain_sum;
	uint32_t bg_sum = ps->cal_bgain_sum;

	memset(ps, 0, sizeof(*ps));
	ps->phase = phase;

	/* Preserve calibration across phase resets */
	ps->calibrated = was_calibrated;
	ps->rgain_base = rg_base;
	ps->bgain_base = bg_base;
	ps->cal_count = cal_count;
	ps->cal_rgain_sum = rg_sum;
	ps->cal_bgain_sum = bg_sum;
}

/*
 * Auto-calibrate AWB R/B gain baseline from the first N stable
 * daytime samples. Only samples where EV indicates bright scene
 * (low ev) are used, ensuring the baseline reflects daylight AWB
 * and not IR-shifted values.
 */
static bool photo_calibrate(ric_state_t *st)
{
	ric_photo_state_t *ps = &st->photo;
	const ric_photo_thresholds_t *thr = &st->settings.photo;

	if (ps->calibrated)
		return true;

	/* Only calibrate from bright samples */
	if (ps->ev > thr->ev_day)
		return false;

	ps->cal_rgain_sum += ps->rgain;
	ps->cal_bgain_sum += ps->bgain;
	ps->cal_count++;

	if (ps->cal_count < PHOTO_CAL_SAMPLES)
		return false;

	ps->rgain_base = (uint16_t)(ps->cal_rgain_sum / PHOTO_CAL_SAMPLES);
	ps->bgain_base = (uint16_t)(ps->cal_bgain_sum / PHOTO_CAL_SAMPLES);
	ps->calibrated = true;

	RSS_INFO("photo AWB calibrated: rgain=%u bgain=%u (%d samples)", ps->rgain_base,
		 ps->bgain_base, PHOTO_CAL_SAMPLES);
	return true;
}

/*
 * Night detection: EV is high (dark), check R/B gain spectral shift.
 *
 * HIGH ev = dark on Ingenic (more exposure needed).
 * R/B gains deviate from daytime baseline when IR illumination
 * changes the scene's spectral composition.
 */
static void photo_night_control(ric_state_t *st)
{
	ric_photo_state_t *ps = &st->photo;
	const ric_photo_thresholds_t *thr = &st->settings.photo;

	ps->settle_count++;
	if (ps->settle_count < 7)
		return;
	if (ps->settle_count > 250)
		ps->settle_count = 250;

	uint16_t rg = ps->rgain;
	uint16_t bg = ps->bgain;
	uint16_t rg_base = ps->rgain_base;
	uint16_t bg_base = ps->bgain_base;
	uint32_t ev = ps->ev;
	int rdiff, bdiff;

	/* R-gain deviation check 1 (threshold 15) */
	rdiff = (int)rg - (int)rg_base;
	if (rdiff < 0)
		rdiff = -rdiff;
	if (rdiff >= RGAIN_DEV_THR1)
		ps->rgain_dev[0]++;
	else
		ps->rgain_dev[0] = 0;

	/* B-gain deviation check 1 (threshold 10) */
	bdiff = (int)bg - (int)bg_base;
	if (bdiff < 0)
		bdiff = -bdiff;
	if (bdiff >= BGAIN_DEV_THR1)
		ps->bgain_dev[0]++;
	else
		ps->bgain_dev[0] = 0;

	/* R-gain deviation check 2 (threshold 9) */
	if (rdiff >= RGAIN_DEV_THR2)
		ps->rgain_dev[1]++;
	else
		ps->rgain_dev[1] = 0;

	/* B-gain deviation check 2 (threshold 7) */
	if (bdiff >= BGAIN_DEV_THR2)
		ps->bgain_dev[1]++;
	else
		ps->bgain_dev[1] = 0;

	/*
	 * EV level checks — HIGH ev = dark on Ingenic.
	 * ev > ev_night: dark enough for night consideration.
	 * ev > ev_deep: very dark.
	 */
	if (ev > thr->ev_night) {
		if (ps->ev_night_count < 250)
			ps->ev_night_count++;

		if (ev > thr->ev_deep) {
			if (ps->ev_deep_count < 250)
				ps->ev_deep_count++;
		} else {
			ps->ev_deep_count = 0;
		}
	} else {
		ps->ev_night_count = 0;
		if (ev <= thr->ev_deep)
			ps->ev_deep_count = 0;
		else if (ps->ev_deep_count < 250)
			ps->ev_deep_count++;
	}

	/*
	 * Night trigger: two independent paths, either can fire.
	 *
	 * Path 1: ev_night counter ≥ 23 AND both R-gain deviation
	 *         counters ≥ 3 (dark + strong spectral shift).
	 * Path 2: ev_deep counter ≥ 23 AND both B-gain deviation
	 *         counters ≥ 7 (very dark + moderate spectral shift).
	 */
	int reason = 0;

	if (ps->ev_night_count >= NIGHT_EV_TRIGGER && ps->rgain_dev[0] >= NIGHT_RGAIN_TRIGGER &&
	    ps->rgain_dev[1] >= NIGHT_RGAIN_TRIGGER)
		reason = 1;

	if (reason == 0 && ps->ev_deep_count >= NIGHT_EV_TRIGGER &&
	    ps->bgain_dev[0] >= NIGHT_BGAIN_TRIGGER && ps->bgain_dev[1] >= NIGHT_BGAIN_TRIGGER)
		reason = 2;

	if (reason > 0) {
		RSS_DEBUG("photo night trigger (reason=%d ev=%u rg=%u bg=%u base=%u/%u)", reason,
			  ev, rg, bg, ps->rgain_base, ps->bgain_base);
		ps->change_pending = true;
		ps->max_dgain = 0;
		ric_set_mode(st, RIC_MODE_NIGHT);
		ric_photo_reset(ps, PHOTO_PHASE_DAY_DETECT);
		return;
	}

	/* Fixed-EV fallback: sustained high ev (dark) → force night */
	ps->fixed_polls++;
	if (ps->fixed_polls >= FIXED_CHECK_INTERVAL) {
		ps->fixed_polls = 0;
		if (ps->fixed_check_ev > 0 && ev > ps->fixed_check_ev - FIXED_CHECK_DRIFT_LIMIT) {
			ps->fixed_check_count++;
		} else {
			ps->fixed_check_count = 0;
		}
		ps->fixed_check_ev = ev;

		if (ps->fixed_check_count >= FIXED_DRIFT_TRIGGER) {
			if (ev >= thr->ev_deep) {
				RSS_DEBUG("photo fixed-ev night (ev=%u)", ev);
				ps->change_pending = true;
				ps->max_dgain = 0;
				ric_set_mode(st, RIC_MODE_NIGHT);
				ric_photo_reset(ps, PHOTO_PHASE_DAY_DETECT);
			}
			ps->fixed_check_count = 0;
		}
	}
}

/*
 * Day detection: collect EV samples and compare against a
 * reference using ratio thresholds.
 *
 * When the scene brightens, EV drops (less exposure needed).
 * The ratio comparison is sensor-independent since it measures
 * relative change, not absolute EV.
 */
static void photo_day_control(ric_state_t *st)
{
	ric_photo_state_t *ps = &st->photo;
	const ric_photo_thresholds_t *thr = &st->settings.photo;

	/* EV must be LOW (bright) to consider day — inverted from Wyze */
	if (ps->ev >= thr->ev_day) {
		ps->day_ring_idx = 0;
		return;
	}

	uint8_t idx = ps->day_ring_idx;
	ps->day_ring[idx % PHOTO_DAY_RING_SIZE] = ps->ev;
	idx++;
	ps->day_ring_idx = idx;

	if (idx < PHOTO_DAY_RING_SIZE)
		return;

	uint32_t sample = ps->day_ring[(idx - 1) % PHOTO_DAY_RING_SIZE];
	double s = (double)sample;
	double ref = (double)ps->day_ref_ev;

	if (ref <= 0.0) {
		ps->day_ring_idx = 0;
		return;
	}

	/*
	 * Ratio check — inverted because LOW ev = bright.
	 * Sample must be within the reference band to count as day.
	 * (In original algorithm high ev = bright, here low ev = bright,
	 * so we check if sample is close to or below the reference.)
	 */
	if (s >= ref * DAY_RATIO_HIGH) {
		ps->day_ring_idx = 0;
		return;
	}

	if (s > ref * DAY_RATIO_LOW) {
		ps->day_ring_idx = 0;
		ps->day_trigger_count++;

		if (ps->day_trigger_count < 3) {
			RSS_DEBUG("photo day approach (%d/3 ev=%u ref=%u)", ps->day_trigger_count,
				  sample, ps->day_ref_ev);
			ps->max_dgain = 1000000;
			ps->change_pending = true;
			return;
		}

		RSS_DEBUG("photo day trigger (ev=%u ref=%u rg=%u bg=%u)", ps->ev, ps->day_ref_ev,
			  ps->rgain, ps->bgain);
		ps->change_pending = true;
		ps->max_dgain = 100000;
		ric_set_mode(st, RIC_MODE_DAY);
		ric_photo_reset(ps, PHOTO_PHASE_INTERFERE);
		return;
	}

	ps->day_ring_idx = 0;
}

/*
 * Anti-interference: after a night→day transition, monitor for
 * rapid EV rises (darkening) that indicate a false trigger
 * (headlights, flashlights passing through the FOV).
 *
 * HIGH ev = dark, so a rise means the scene is getting darker.
 */
static void photo_interfere_control(ric_state_t *st)
{
	ric_photo_state_t *ps = &st->photo;

	ps->interf_polls++;

	if (ps->interf_polls > INTERF_MAX_POLLS) {
		RSS_DEBUG("photo interfere timeout, resetting to night detect");
		ric_photo_reset(ps, PHOTO_PHASE_NIGHT_DETECT);
		return;
	}

	uint8_t idx = ps->interf_ring_idx;
	if (idx < PHOTO_INTERF_RING_SIZE) {
		ps->interf_ring[idx] = ps->ev;
		ps->interf_ring_idx = idx + 1;

		if (idx + 1 < PHOTO_INTERF_RING_SIZE)
			return;

		uint32_t first = ps->interf_ring[0];
		uint32_t last = ps->interf_ring[PHOTO_INTERF_RING_SIZE - 1];
		double df = (double)first;
		double dl = (double)last;

		if (dl < df * FIXED_RATIO_LOW || dl > df * FIXED_RATIO_HIGH) {
			ps->interf_ring_idx = 0;
			return;
		}
		ps->interf_ref_ev = last;
		return;
	}

	if (ps->interf_ref_ev == 0)
		return;

	double current = (double)ps->ev;
	double ref = (double)ps->interf_ref_ev;

	/* EV rose (darker) — possible false day trigger */
	if (current > ref * INTERF_RATIO_HIGH) {
		ps->interf_rise_count++;
		if (ps->interf_rise_count > INTERF_RISE_TRIGGER) {
			RSS_DEBUG("photo interfere: false day (ev=%u ref=%u)", ps->ev,
				  ps->interf_ref_ev);
			ps->change_pending = true;
			ps->max_dgain = 100000;
			ric_set_mode(st, RIC_MODE_NIGHT);
			ric_photo_reset(ps, PHOTO_PHASE_NIGHT_DETECT);
			return;
		}
		if (current < ref * INTERF_RATIO_LOW)
			ps->interf_fall_count = 0;
		return;
	}

	if (current >= ref * INTERF_RATIO_LOW)
		ps->interf_rise_count = 0;

	/* EV dropped (brighter) — scene genuinely brightening */
	if (current < ref * INTERF_RATIO_LOW) {
		ps->interf_fall_count++;
		if (ps->interf_fall_count >= INTERF_FALL_TRIGGER) {
			RSS_DEBUG("photo interfere: genuine bright (ev=%u ref=%u)", ps->ev,
				  ps->interf_ref_ev);
			ric_photo_reset(ps, PHOTO_PHASE_NIGHT_DETECT);
		}
		return;
	}

	ps->interf_fall_count = 0;
}

/*
 * Fixed-EV drift detection: during night mode, check if EV is
 * dropping (brightening). Sustained drop → scene got brighter,
 * switch to day.
 */
static void photo_fixed_control(ric_state_t *st)
{
	ric_photo_state_t *ps = &st->photo;

	uint8_t idx = ps->fixed_ring_idx;
	if (idx < PHOTO_FIXED_RING_SIZE) {
		ps->fixed_ring[idx] = ps->ev;
		ps->fixed_ring_idx = idx + 1;

		if (idx + 1 < PHOTO_FIXED_RING_SIZE)
			return;

		uint32_t first = ps->fixed_ring[0];
		uint32_t last = ps->fixed_ring[PHOTO_FIXED_RING_SIZE - 1];
		double df = (double)first;
		double dl = (double)last;

		if (dl < df * FIXED_RATIO_LOW || dl > df * FIXED_RATIO_HIGH) {
			ps->fixed_ring_idx = 0;
			return;
		}
		ps->fixed_ref_ev = last;
		return;
	}

	if (ps->fixed_ref_ev == 0)
		return;

	double current = (double)ps->ev;
	double ref = (double)ps->fixed_ref_ev;

	/* EV rising (darker) or stable → no drift */
	if (current >= ref * FIXED_DRIFT_THR) {
		ps->fixed_drift_count = 0;
		return;
	}

	/* EV dropped below drift threshold → brightening */
	ps->fixed_drift_count++;
	if (ps->fixed_drift_count > FIXED_DRIFT_TRIGGER) {
		RSS_DEBUG("photo fixed drift: day detected (ev=%u ref=%u)", ps->ev,
			  ps->fixed_ref_ev);
		ps->fixed_drift_count = 0;
		ps->change_pending = true;
		ric_set_mode(st, RIC_MODE_DAY);
		ric_photo_reset(ps, PHOTO_PHASE_NIGHT_DETECT);
	}
}

/*
 * Main photo mode dispatcher.
 */
void ric_photo_poll(ric_state_t *st, uint32_t ev, uint16_t rgain, uint16_t bgain)
{
	static const char *phase_names[] = {"?", "night_detect", "day_detect", "interfere"};
	static uint32_t poll_count;
	ric_photo_state_t *ps = &st->photo;

	if (st->settings.opmode != RIC_AUTO)
		return;

	ps->ev = ev;
	ps->rgain = rgain;
	ps->bgain = bgain;
	poll_count++;

	/* Auto-calibrate R/B gain baseline from bright samples */
	if (!ps->calibrated) {
		if (!photo_calibrate(st))
			return;
	}

	/* Anti-flap: after mode transitions, wait for ISP to settle */
	if (ps->anti_flap) {
		ps->anti_flap_ticks++;
		if (ps->anti_flap_ticks > ANTI_FLAP_SETTLE) {
			ps->anti_flap = false;
			ps->anti_flap_ticks = 0;
			ps->anti_flap_count = 0;
		}
		return;
	}

	if (poll_count % 10 == 0) {
		RSS_DEBUG("photo [%s] ev=%u rg=%u bg=%u (base=%u/%u) | "
			  "night=%u deep=%u rd=%u/%u bd=%u/%u",
			  phase_names[ps->phase], ev, rgain, bgain, ps->rgain_base, ps->bgain_base,
			  ps->ev_night_count, ps->ev_deep_count, ps->rgain_dev[0], ps->rgain_dev[1],
			  ps->bgain_dev[0], ps->bgain_dev[1]);
	}

	switch (ps->phase) {
	case PHOTO_PHASE_NIGHT_DETECT:
		photo_night_control(st);
		break;
	case PHOTO_PHASE_DAY_DETECT:
		photo_day_control(st);
		if (st->current_mode == RIC_MODE_NIGHT)
			photo_fixed_control(st);
		break;
	case PHOTO_PHASE_INTERFERE:
		photo_interfere_control(st);
		break;
	}
}

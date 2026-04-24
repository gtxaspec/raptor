/*
 * ric_photo.c -- Multi-stage EV + AWB day/night detection
 *
 * Uses ISP EV value and white balance R/B gain statistics through
 * a multi-phase state machine with anti-interference filtering.
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
#define NIGHT_6LUX_TRIGGER  23
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
	ps->phase = phase;
	ps->settle_count = 0;
	ps->rgain_dev[0] = 0;
	ps->rgain_dev[1] = 0;
	ps->bgain_dev[0] = 0;
	ps->bgain_dev[1] = 0;
	ps->ev_3lux_count = 0;
	ps->ev_6lux_count = 0;
	ps->day_ring_idx = 0;
	ps->day_ref_ev = 0;
	ps->day_trigger_count = 0;
	ps->interf_ring_idx = 0;
	ps->interf_ref_ev = 0;
	ps->interf_polls = 0;
	ps->interf_rise_count = 0;
	ps->interf_fall_count = 0;
	ps->fixed_ring_idx = 0;
	ps->fixed_ref_ev = 0;
	ps->fixed_drift_count = 0;
	ps->fixed_polls = 0;
	ps->fixed_check_ev = 0;
	ps->fixed_check_count = 0;
	ps->anti_flap = false;
	ps->anti_flap_count = 0;
	ps->anti_flap_ticks = 0;
	ps->change_pending = false;
	ps->max_dgain = 0;
}

/*
 * Night detection: EV is low (dark), check R/B gain spectral shift.
 *
 * R-gain and B-gain deviate from the sensor's baseline under IR
 * illumination. Multiple deviation checks with different thresholds
 * vote on whether the scene is genuinely dark vs IR-contaminated.
 * After sustained low EV + gain deviation → switch to night.
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
	uint16_t rg_rec = thr->rgain_rec;
	uint16_t bg_rec = thr->bgain_rec;
	uint32_t ev = ps->ev;
	int rdiff, bdiff;

	/* R-gain deviation check 1 (threshold 15) */
	rdiff = (int)rg - (int)rg_rec;
	if (rdiff < 0)
		rdiff = -rdiff;
	if (rdiff >= RGAIN_DEV_THR1)
		ps->rgain_dev[0]++;
	else
		ps->rgain_dev[0] = 0;

	/* B-gain deviation check 1 (threshold 10) */
	bdiff = (int)bg - (int)bg_rec;
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

	/* EV level checks */
	if (ev < thr->ev_3lux) {
		if (ps->ev_3lux_count < 250)
			ps->ev_3lux_count++;

		if (ev < thr->ev_6lux) {
			if (ps->ev_6lux_count < 250)
				ps->ev_6lux_count++;
		} else {
			ps->ev_6lux_count = 0;
		}
	} else {
		ps->ev_3lux_count = 0;
		if (ev >= thr->ev_6lux)
			ps->ev_6lux_count = 0;
		else if (ps->ev_6lux_count < 250)
			ps->ev_6lux_count++;
	}

	/*
	 * Night trigger: two independent paths, either can fire.
	 *
	 * Path 1: 6lux counter ≥ 23 AND both R-gain deviation
	 *         counters ≥ 3 (strong spectral + dark).
	 * Path 2: 3lux counter ≥ 23 AND both B-gain deviation
	 *         counters ≥ 7 (moderate spectral + very dark).
	 */
	int reason = 0;

	if (ps->ev_3lux_count >= NIGHT_6LUX_TRIGGER && ps->rgain_dev[0] >= NIGHT_RGAIN_TRIGGER &&
	    ps->rgain_dev[1] >= NIGHT_RGAIN_TRIGGER)
		reason = 1;

	if (reason == 0 && ps->ev_6lux_count >= NIGHT_6LUX_TRIGGER &&
	    ps->bgain_dev[0] >= NIGHT_BGAIN_TRIGGER && ps->bgain_dev[1] >= NIGHT_BGAIN_TRIGGER)
		reason = 2;

	if (reason > 0) {
		RSS_DEBUG("photo night trigger (reason=%d ev=%u rg=%u bg=%u)", reason, ev, rg, bg);
		ps->change_pending = true;
		ps->max_dgain = 0;
		ric_set_mode(st, RIC_MODE_NIGHT);
		ric_photo_reset(ps, PHOTO_PHASE_DAY_DETECT);
		ps->settle_count = 0;
		return;
	}

	/* Fixed-EV fallback: sustained very dark → force night */
	ps->fixed_polls++;
	if (ps->fixed_polls >= FIXED_CHECK_INTERVAL) {
		ps->fixed_polls = 0;
		if (ps->fixed_check_ev > 0 && ev < ps->fixed_check_ev + FIXED_CHECK_DRIFT_LIMIT) {
			ps->fixed_check_count++;
		} else {
			ps->fixed_check_count = 0;
		}
		ps->fixed_check_ev = ev;

		if (ps->fixed_check_count >= FIXED_DRIFT_TRIGGER) {
			if (ev <= thr->ev_1lux) {
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
 * When the scene brightens (dawn, lights on), EV rises relative to
 * the dark-mode baseline. The ratio comparison is sensor-independent
 * since it measures relative change, not absolute EV.
 */
static void photo_day_control(ric_state_t *st)
{
	ric_photo_state_t *ps = &st->photo;
	const ric_photo_thresholds_t *thr = &st->settings.photo;

	if (ps->ev <= thr->ev_day) {
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

	if (s <= ref * DAY_RATIO_LOW) {
		ps->day_ring_idx = 0;
		return;
	}

	if (s < ref * DAY_RATIO_HIGH) {
		ps->day_ring_idx = 0;
		ps->day_trigger_count++;

		if (ps->day_trigger_count < 3) {
			RSS_DEBUG("photo day approach (%d/3 ev=%u ref=%u)", ps->day_trigger_count,
				  sample, ps->day_ref_ev);
			ps->phase = PHOTO_PHASE_DAY_DETECT;
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
	return;
}

/*
 * Anti-interference: after a night→day transition, monitor for
 * rapid EV drops that indicate a false trigger (headlights,
 * flashlights passing through the FOV).
 *
 * Collects an 8-sample EV baseline then watches for sustained
 * drops below the reference. Three consecutive drops revert to
 * night mode. Times out after 9000 polls (~2.5 hours at 1s).
 */
static void photo_interfere_control(ric_state_t *st)
{
	ric_photo_state_t *ps = &st->photo;

	ps->interf_polls++;

	if (ps->interf_polls > INTERF_MAX_POLLS) {
		RSS_DEBUG("photo interfere timeout, resetting to night detect");
		ps->interf_rise_count = 0;
		ps->interf_fall_count = 0;
		ric_photo_reset(ps, PHOTO_PHASE_NIGHT_DETECT);
		return;
	}

	uint8_t idx = ps->interf_ring_idx;
	if (idx < PHOTO_INTERF_RING_SIZE) {
		ps->interf_ring[idx] = ps->ev;
		ps->interf_ring_idx = idx + 1;

		if (idx + 1 < PHOTO_INTERF_RING_SIZE)
			return;

		/* Baseline collected: validate stability */
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

	if (current > ref * INTERF_RATIO_HIGH) {
		/* EV rose well above reference — possible false day trigger */
		ps->interf_rise_count++;
		if (ps->interf_rise_count > INTERF_RISE_TRIGGER) {
			ps->interf_rise_count = 0;
			ps->interf_fall_count = 0;
			RSS_DEBUG("photo interfere: false day (ev=%u ref=%u)", ps->ev,
				  ps->interf_ref_ev);
			ps->change_pending = true;
			ps->max_dgain = 100000;
			ric_set_mode(st, RIC_MODE_NIGHT);
			ric_photo_reset(ps, PHOTO_PHASE_NIGHT_DETECT);
			return;
		}
		if (current > ref * INTERF_RATIO_LOW)
			ps->interf_fall_count = 0;
		return;
	}

	if (current <= ref * INTERF_RATIO_LOW) {
		ps->interf_rise_count = 0;
	}

	if (current < ref * INTERF_RATIO_LOW) {
		/* EV dropped below reference — scene genuinely darkening */
		ps->interf_fall_count++;
		if (ps->interf_fall_count >= INTERF_FALL_TRIGGER) {
			RSS_DEBUG("photo interfere: genuine dark (ev=%u ref=%u)", ps->ev,
				  ps->interf_ref_ev);
			ric_photo_reset(ps, PHOTO_PHASE_NIGHT_DETECT);
		}
		return;
	}

	ps->interf_fall_count = 0;
}

/*
 * Fixed-EV drift detection: during night mode, check if EV is
 * drifting down from the baseline (sensor stabilizing or scene
 * getting darker). If EV consistently drops, the fixed baseline
 * is stale — update it.
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

		/* Validate baseline stability */
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

	if (current >= ref * FIXED_DRIFT_THR) {
		ps->fixed_drift_count = 0;
		return;
	}

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
 * Main photo mode dispatcher. Called each poll interval.
 * Runs the EV/gain judges then dispatches to the current
 * phase handler.
 */
void ric_photo_poll(ric_state_t *st, uint32_t ev, uint16_t rgain, uint16_t bgain)
{
	ric_photo_state_t *ps = &st->photo;

	if (st->settings.opmode != RIC_AUTO)
		return;

	ps->ev = ev;
	ps->rgain = rgain;
	ps->bgain = bgain;

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

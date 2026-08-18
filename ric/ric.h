/*
 * ric.h -- RIC internal state
 */

#ifndef RIC_H
#define RIC_H

#include <stdbool.h>
#include <stdint.h>

#include <rss_ipc.h>
#include <rss_common.h>

/* Day/night state */
typedef enum {
	RIC_MODE_UNSET = -1, /* startup only: forces the first set through */
	RIC_MODE_DAY = 0,
	RIC_MODE_NIGHT = 1,
} ric_mode_t;

/* Operating mode */
typedef enum {
	RIC_AUTO = 0,
	RIC_FORCE_DAY = 1,
	RIC_FORCE_NIGHT = 2,
} ric_opmode_t;

/* Trigger mode for day/night detection */
typedef enum {
	RIC_TRIGGER_LUMA = 0,  /* ae_luma + gain ratio (sensor-independent, default) */
	RIC_TRIGGER_GAIN = 1,  /* total_gain fixed thresholds (sensor-dependent, legacy) */
	RIC_TRIGGER_ADC = 2,   /* SU_ADC photoresistor (hardware LDR, most reliable) */
	RIC_TRIGGER_PHOTO = 3, /* EV + AWB multi-stage with anti-interference */
} ric_trigger_t;

/* Photo mode detection phases */
typedef enum {
	PHOTO_PHASE_NIGHT_DETECT = 1,
	PHOTO_PHASE_DAY_DETECT = 2,
	PHOTO_PHASE_INTERFERE = 3,
} ric_photo_phase_t;

#define PHOTO_DAY_RING_SIZE    10
#define PHOTO_INTERF_RING_SIZE 8
#define PHOTO_FIXED_RING_SIZE  8

/*
 * Photo mode thresholds.
 *
 * EV direction: on Ingenic, ev = integration_time × gain product.
 * HIGH ev = dark (more exposure needed), LOW ev = bright.
 * Thresholds are compared with > for darkness, < for brightness.
 */
typedef struct {
	uint32_t ev_night;  /* EV above this → dark (default 50000) */
	uint32_t ev_deep;   /* EV above this → very dark (default 150000) */
	uint32_t ev_day;    /* EV below this → bright (default 5000) */
	uint16_t rgain_rec; /* R-gain baseline (0 = auto-calibrate) */
	uint16_t bgain_rec; /* B-gain baseline (0 = auto-calibrate) */
} ric_photo_thresholds_t;

/* Auto-calibration sample count */
#define PHOTO_CAL_SAMPLES 16

/* Photo mode runtime state */
typedef struct {
	ric_photo_phase_t phase;

	/* Current sensor readings */
	uint32_t ev;
	uint16_t rgain;
	uint16_t bgain;

	/* AWB baseline auto-calibration */
	bool calibrated;
	uint8_t cal_count;
	uint32_t cal_first_ev; /* EV of the window's first sample: the whole
				* window must agree with it to +-13% */
	uint32_t cal_rgain_sum;
	uint32_t cal_bgain_sum;
	uint16_t rgain_base;
	uint16_t bgain_base;

	/* Night detection (phase == NIGHT_DETECT) */
	uint8_t settle_count;
	uint8_t rgain_dev[2];
	uint8_t bgain_dev[2];
	uint8_t ev_night_count;
	uint8_t ev_deep_count;

	/* Day detection (phase == DAY_DETECT). The index counts on past
	 * ring-full so it must not wrap back under the size and replay
	 * the seeding return. */
	uint32_t day_ring_idx;
	uint32_t day_ring[PHOTO_DAY_RING_SIZE];
	uint32_t day_ref_ev;
	uint8_t day_trigger_count;

	/* Anti-interference (phase == INTERFERE) */
	uint8_t interf_ring_idx;
	uint32_t interf_ring[PHOTO_INTERF_RING_SIZE];
	uint32_t interf_ref_ev;
	uint32_t interf_polls;
	uint8_t interf_rise_count;
	uint8_t interf_fall_count;

	/* Fixed-EV drift (runs in parallel during night) */
	uint8_t fixed_ring_idx;
	uint32_t fixed_ring[PHOTO_FIXED_RING_SIZE];
	uint32_t fixed_ref_ev;
	uint8_t fixed_drift_count;
	uint32_t fixed_polls;
	uint32_t fixed_check_ev;
	uint8_t fixed_check_count;

} ric_photo_state_t;

/* Config from [ircut] section */
typedef struct {
	bool enabled;
	ric_opmode_t opmode;

	/* GPIO pins (-1 = not used) */
	int gpio_ircut;	 /* IR-cut filter pin (single GPIO mode) */
	int gpio_ircut2; /* second pin for dual GPIO mode, -1 = single */
	int gpio_irled;	 /* IR LED enable pin (ir850) */
	int gpio_irled2; /* second IR LED pin (ir940), -1 = none */
	/*
	 * Inverted drive, single-pin forms only: a dual-pin ircut
	 * expresses polarity by pin order (first = day, second = night)
	 * and never needs these. Inversion happens here in userspace;
	 * the sysfs active_low attribute is deliberately not used.
	 */
	bool ircut_active_low;	/* single-pin ircut driven inverted */
	bool irled_active_low;	/* ir850 bank lights on 0 */
	bool irled2_active_low; /* ir940 bank lights on 0 */
	bool ir850_enabled;
	bool ir940_enabled;
	bool ir940_explicit; /* config carries an ir940 key (vs the default) */

	/* Trigger mode */
	ric_trigger_t trigger;

	/* Luma trigger thresholds */
	int night_luma;	       /* ae_luma below this → night (default 20, 0-255) */
	int night_gain;	       /* gain above this → night regardless of luma (default 80000) */
	int day_gain_pct;      /* night→day: gain below this % of baseline → day (default 25) */
	int probe_gain_pct;    /* night: gain dip below this % of baseline lifts the IR
				* LEDs for an ambient luma probe (default 90, 0 = off) */
	int probe_holdoff_sec; /* minimum spacing between failed probes (default 60) */
	int probe_recheck_sec; /* probe anyway after this long in night with no dip:
				* IR wash can hide ambient light from every AE
				* metric (default 600, 0 = off) */

	/* Gain trigger thresholds (legacy, trigger=gain only) */
	int night_threshold; /* gain above this → night */
	int day_threshold;   /* gain below this → day */

	/* ADC trigger (trigger=adc only) */
	int adc_channel; /* SU_ADC channel number (default 0) */
	int adc_night;	 /* ADC value below this → night (default 200) */
	int adc_day;	 /* ADC value above this → day (default 600) */

	/* Photo trigger thresholds */
	ric_photo_thresholds_t photo;

	int hysteresis_sec; /* consecutive seconds before switching */

	/* Night sensor rate (0 = off). Dropping the sensor rate at night
	 * raises the exposure ceiling -- frame period bounds integration
	 * time -- so AE trades gain (noise) for photons. Applied through
	 * rvd's transient set-sensor-fps: never persisted, restored on
	 * day. The luma trigger is rate-safe by construction (its night
	 * baselines are sampled inside the night regime); the legacy gain
	 * trigger's absolute day_threshold must be calibrated with this
	 * enabled, since night readings then live at the night rate. */
	int night_fps;

	/* Sample interval */
	int poll_interval_ms;

	/* Dual-GPIO IR-cut coil pulse width */
	int pulse_ms;
	bool pulse_ms_explicit; /* raptor.conf overrides device metadata */
} ric_config_t;

/* Global state */
typedef struct {
	ric_config_t settings;

	/* Current state */
	ric_mode_t current_mode;
	int day_count;	 /* consecutive samples below day_threshold */
	int night_count; /* consecutive samples above night_threshold */

	/* Anti-flap: cooldown after mode switch + gain baseline */
	int cooldown_remaining;	      /* polls remaining before evaluating transitions */
	uint32_t night_gain_baseline; /* total_gain sampled after IR LEDs stabilize */
	uint32_t night_ev_baseline;   /* EV at the same settled moment: the probe dip
				       * watches EV where it exists, because AE can
				       * answer new light with exposure alone while
				       * gain sits pinned at its floor (Wyze V3) */
	uint32_t night_detect_gain;   /* gain at the moment night was detected */

	/* Ratio-triggered day switches are verified once the IR is off:
	 * a covered lens under IR is optically identical to a bright
	 * scene, and only the post-switch reading separates them. */
	bool day_verify_pending;
	int day_lockout_polls; /* suppress day attempts while > 0 */
	int day_lockout_next;  /* doubling backoff, 0 = start over */

	/* IR-off ambient probe (night, luma trigger): compressed-gain
	 * sensors floor total_gain in a lit scene long before the day
	 * ratio can fire; the dip below the baseline is the hint, the
	 * probe lifts the LEDs so luma becomes trustworthy. */
	bool probe_active;
	int probe_polls_left;	 /* settle + evaluation window countdown */
	int probe_holdoff_polls; /* suppress probes while > 0 */
	int probe_dip_run;	 /* consecutive dip polls before a probe fires */
	int probe_recheck_polls; /* countdown to the next interval recheck */

	/* Baseline settling: gc2053-class AE walks for many seconds after
	 * the IR lights the scene; the cooldown extends until three
	 * consecutive polls agree within 10% (walks step and can hold a
	 * value briefly), within a hard cap, so the baseline reflects a
	 * settled reading instead of a mid-walk value. */
	uint32_t settle_prev_gain;
	int settle_agree_run;
	int settle_extend_left;

	/* Photo mode state */
	ric_photo_state_t photo;

	/* ADC state */
	int adc_fd;
	bool adc_initialized;
	int adc_fail_run;     /* consecutive failed reads, 0 while healthy */
	bool adc_fail_warned; /* a warning has fired for the current run */

	/* rvd outage accounting, same shape as the ADC run above */
	int rvd_fail_run;
	bool rvd_fail_warned;

	/* night_fps found unusable at runtime (rvd backend cannot report
	 * or set the sensor rate); warned once, feature parked until
	 * restart so the transition path stops hammering a dead verb */
	bool night_fps_unusable;

	/* One-shot diagnostics: a missing signal is worth saying once, and
	 * saying every poll instead is how a log stops being read. */
	bool no_exposure_warned;

	/* Control */
	rss_ctrl_t *ctrl;
	rss_config_t *cfg;
	const char *config_path;

	volatile sig_atomic_t *running;
} ric_state_t;

/* ric_daynight.c */
void ric_gpio_init(ric_state_t *st);
void ric_set_mode(ric_state_t *st, ric_mode_t mode);
void ric_trigger_rearm(ric_state_t *st);
void ric_force_mode(ric_state_t *st, ric_mode_t mode);
void ric_set_isp_mode(ric_mode_t mode);
void ric_apply_night_fps(ric_state_t *st, ric_mode_t mode);
int ric_ircut_drive(ric_state_t *st, ric_mode_t pos);
int ric_irled_drive(ric_state_t *st, bool bank940, bool on);
void ric_poll_exposure(ric_state_t *st);
bool ric_adc_start(ric_state_t *st);
void ric_adc_cleanup(ric_state_t *st);

/* ric_photo.c */
void ric_photo_reset(ric_photo_state_t *ps, ric_photo_phase_t phase);
void ric_photo_poll(ric_state_t *st, uint32_t ev, uint16_t rgain, uint16_t bgain);

#endif /* RIC_H */

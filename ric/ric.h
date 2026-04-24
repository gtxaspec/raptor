/*
 * ric.h -- RIC internal state
 */

#ifndef RIC_H
#define RIC_H

#include <rss_ipc.h>
#include <rss_common.h>

#include <stdbool.h>
#include <stdint.h>

/* Day/night state */
typedef enum {
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

/* Photo mode thresholds */
typedef struct {
	uint32_t ev_day;    /* EV above this starts day detection (default 188000) */
	uint32_t ev_1lux;   /* EV below this forces night after drift (default 10000) */
	uint32_t ev_3lux;   /* EV below this increments 3lux night counter (default 3000) */
	uint32_t ev_6lux;   /* EV below this increments 6lux night counter (default 1200) */
	uint16_t rgain_rec; /* R-gain record threshold (default 250) */
	uint16_t bgain_rec; /* B-gain record threshold (default 187) */
} ric_photo_thresholds_t;

/* Photo mode runtime state */
typedef struct {
	ric_photo_phase_t phase;

	/* Current sensor readings */
	uint32_t ev;
	uint16_t rgain;
	uint16_t bgain;

	/* Night detection (phase == NIGHT_DETECT) */
	uint8_t settle_count;
	uint8_t rgain_dev[2];
	uint8_t bgain_dev[2];
	uint8_t ev_3lux_count;
	uint8_t ev_6lux_count;

	/* Day detection (phase == DAY_DETECT) */
	uint8_t day_ring_idx;
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

	/* Anti-flap */
	bool anti_flap;
	uint8_t anti_flap_count;
	uint32_t anti_flap_ticks;

	/* Pending mode change */
	bool change_pending;
	uint32_t max_dgain;
} ric_photo_state_t;

/* Config from [ircut] section */
typedef struct {
	bool enabled;
	ric_opmode_t opmode;

	/* GPIO pins (-1 = not used) */
	int gpio_ircut;	 /* IR-cut filter pin (single GPIO mode) */
	int gpio_ircut2; /* second pin for dual GPIO mode, -1 = single */
	int gpio_irled;	 /* IR LED enable pin */

	/* Trigger mode */
	ric_trigger_t trigger;

	/* Luma trigger thresholds */
	int night_luma;	  /* ae_luma below this → night (default 20, 0-255) */
	int night_gain;	  /* gain above this → night regardless of luma (default 80000) */
	int day_gain_pct; /* night→day: gain below this % of baseline → day (default 25) */

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

	/* Sample interval */
	int poll_interval_ms;
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

	/* Photo mode state */
	ric_photo_state_t photo;

	/* ADC state */
	bool adc_initialized;

	/* Control */
	rss_ctrl_t *ctrl;
	rss_config_t *cfg;
	const char *config_path;

	volatile sig_atomic_t *running;
} ric_state_t;

/* ric_daynight.c */
void ric_gpio_init(ric_state_t *st);
void ric_set_mode(ric_state_t *st, ric_mode_t mode);
void ric_set_isp_mode(ric_mode_t mode);
void ric_poll_exposure(ric_state_t *st);
bool ric_adc_start(ric_state_t *st);
void ric_adc_cleanup(ric_state_t *st);

/* ric_photo.c */
void ric_photo_reset(ric_photo_state_t *ps, ric_photo_phase_t phase);
void ric_photo_poll(ric_state_t *st, uint32_t ev, uint16_t rgain, uint16_t bgain);

#endif /* RIC_H */

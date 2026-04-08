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
	RIC_TRIGGER_LUMA = 0, /* ae_luma + gain ratio (sensor-independent, default) */
	RIC_TRIGGER_GAIN = 1, /* total_gain fixed thresholds (sensor-dependent, legacy) */
	RIC_TRIGGER_ADC = 2,  /* SU_ADC photoresistor (hardware LDR, most reliable) */
} ric_trigger_t;

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

	int hysteresis_sec; /* consecutive seconds before switching */

	/* Sample interval */
	int poll_interval_ms;
} ric_config_t;

/* Global state */
typedef struct {
	ric_config_t cfg;

	/* Current state */
	ric_mode_t current_mode;
	int day_count;	 /* consecutive samples below day_threshold */
	int night_count; /* consecutive samples above night_threshold */

	/* Anti-flap: cooldown after mode switch + gain baseline */
	int cooldown_remaining;	      /* polls remaining before evaluating transitions */
	uint32_t night_gain_baseline; /* total_gain sampled after IR LEDs stabilize */

	/* ADC state */
	bool adc_initialized;

	/* Control */
	rss_ctrl_t *ctrl;
	rss_config_t *config;
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

#endif /* RIC_H */

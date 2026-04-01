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
	RIC_TRIGGER_LUMA = 0, /* ae_luma (0-255, sensor-independent, default) */
	RIC_TRIGGER_GAIN = 1, /* total_gain (sensor-dependent, legacy) */
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

	/* Luma thresholds (ae_luma 0-255, used when trigger=luma) */
	int night_luma; /* luma below this → night (default 20) */
	int day_luma;	/* luma above this → day (default 40) */

	/* Gain thresholds (total_gain, used when trigger=gain) */
	int night_threshold; /* gain above this → night */
	int day_threshold;   /* gain below this → day */

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

	/* Control */
	rss_ctrl_t *ctrl;
	rss_config_t *config;
	const char *config_path;

	volatile sig_atomic_t *running;
} ric_state_t;

/* ric_daynight.c */
void ric_gpio_init(ric_state_t *st);
void ric_set_mode(ric_state_t *st, ric_mode_t mode);
void ric_poll_exposure(ric_state_t *st);

#endif /* RIC_H */

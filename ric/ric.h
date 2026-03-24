/*
 * ric.h -- RIC internal state
 */

#ifndef RIC_H
#define RIC_H

#include <raptor_hal.h>
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

/* Config from [ircut] section */
typedef struct {
	bool enabled;
	ric_opmode_t opmode;

	/* GPIO pins (-1 = not used) */
	int gpio_ircut;	 /* IR-cut filter pin (single GPIO mode) */
	int gpio_ircut2; /* second pin for dual GPIO mode, -1 = single */
	int gpio_irled;	 /* IR LED enable pin */

	/* Thresholds (total_gain from ISP) */
	int night_threshold; /* gain above this → night */
	int day_threshold;   /* gain below this → day */
	int hysteresis_sec;  /* consecutive seconds before switching */

	/* Sample interval */
	int poll_interval_ms;
} ric_config_t;

/* Global state */
typedef struct {
	ric_config_t cfg;

	/* HAL */
	rss_hal_ctx_t *hal_ctx;
	const rss_hal_ops_t *ops;

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

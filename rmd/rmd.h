/*
 * rmd.h -- RMD (Raptor Motion Daemon) internal state
 */

#ifndef RMD_H
#define RMD_H

#include <rss_ipc.h>
#include <rss_common.h>

#include <stdbool.h>
#include <stdint.h>

/* Motion state machine */
typedef enum {
	RMD_STATE_IDLE = 0,
	RMD_STATE_ACTIVE = 1,
	RMD_STATE_COOLDOWN = 2,
} rmd_state_t;

/* Config from [motion] section */
typedef struct {
	bool enabled;
	int sensitivity;
	int cooldown_sec;
	int poll_interval_ms;

	/* Actions */
	bool record_on_motion;
	int record_post_sec;
	int gpio_pin;
} rmd_config_t;

/* Daemon context */
typedef struct {
	rmd_config_t cfg;

	rmd_state_t state;
	int64_t last_motion_us;
	int64_t cooldown_start_us;

	bool recording_active;

	rss_ctrl_t *ctrl;
	rss_config_t *config;
	const char *config_path;

	volatile sig_atomic_t *running;
} rmd_ctx_t;

/* rmd_actions.c */
void rmd_gpio_init(rmd_ctx_t *ctx);
void rmd_gpio_set(rmd_ctx_t *ctx, bool active);
int rmd_trigger_recording(rmd_ctx_t *ctx, bool start);

#endif /* RMD_H */

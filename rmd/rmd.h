/*
 * rmd.h -- RMD (Raptor Motion Daemon) internal state
 */

#ifndef RMD_H
#define RMD_H

#include <rss_ipc.h>
#include <rss_common.h>

#include <stdbool.h>
#include <stdint.h>

#include "rmd_track.h"

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

	int skip_frames;

	/* Actions */
	bool record_on_motion;
	int record_post_sec;
	int gpio_pin;
} rmd_config_t;

/* Config from [track] section: follow a detection with the PTZ head */
typedef struct {
	bool enabled;
	bool shadow;	     /* log the moves, do not send them */
	rmd_track_cfg_t aim; /* frame_w and frame_h come from rvd */
	int settle_ms;	     /* boxes from frames before this long after a stop are ignored */
	int average;	     /* results averaged into one move */
	int speed;	     /* motor speed, 0 = the head's own */
	int lost_sec;	     /* forget the target after this long unseen */
	bool home_on_idle;   /* go back when motion ends */
	bool invert_x;
	bool invert_y;
	char motors_socket[108]; /* motors-daemon's socket */
} rmd_track_settings_t;

/* What the tracker reads and drives: rvd's detections and the head. The real one is
 * rmd_track_io_real; tests give their own. `user` is passed back to each call. */
typedef struct {
	int (*detections)(void *user, char *buf, int size); /* ivs-detections reply, <0 none */
	int (*busy)(void *user);			    /* 1 running, 0 stopped, <0 no answer */
	int (*move)(void *user, int dx, int dy, int speed);
	int (*go_back)(void *user);
} rmd_track_io_t;

/* Tracker state */
typedef struct {
	bool moving;
	bool moved; /* since the last go back */
	int64_t stop_us;
	int64_t last_ts; /* the newest detection result acted on */
	bool have_prev;
	float prev_x, prev_y;
	int64_t last_seen_us;
	int moves;
	rmd_track_avg_t avg; /* aim points since the last stop */
} rmd_track_state_t;

/* Daemon context */
typedef struct {
	rmd_config_t settings;
	rmd_track_settings_t track_cfg;
	rmd_track_state_t track;
	const rmd_track_io_t *track_io; /* NULL: rmd_track_io_real with this context */
	void *track_user;

	rmd_state_t state;
	int64_t last_motion_us;
	int64_t cooldown_start_us;

	bool recording_active;
	int person_count;

	rss_ctrl_t *ctrl;
	rss_config_t *cfg;
	const char *config_path;

	volatile sig_atomic_t *running;
} rmd_ctx_t;

/* rmd_actions.c */
void rmd_gpio_init(rmd_ctx_t *ctx);
void rmd_gpio_set(rmd_ctx_t *ctx, bool active);
int rmd_trigger_recording(rmd_ctx_t *ctx, bool start);

/* rmd_ptz.c: motors-daemon at socket path `sock` */
int rmd_ptz_move(const char *sock, int dx, int dy, int speed);
int rmd_ptz_busy(const char *sock); /* 1 running, 0 stopped, -1 no answer */
int rmd_ptz_go_back(const char *sock);

/* rmd_tracker.c */
extern const rmd_track_io_t rmd_track_io_real; /* user: the rmd_ctx_t */
void rmd_track_load(rmd_ctx_t *ctx);
void rmd_track_step(rmd_ctx_t *ctx);
void rmd_track_shutdown(rmd_ctx_t *ctx); /* the head goes back if the tracker moved it */

#endif /* RMD_H */

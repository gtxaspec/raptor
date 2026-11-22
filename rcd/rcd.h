/*
 * rcd.h -- Raptor Camera Daemon: core <-> driver interface
 *
 * The core owns everything that does not depend on the kind of camera:
 * config, the per-camera thread, the sensor rings and how frames enter
 * them (timestamps, src_seq, UTC mapping, stream info), stall detection,
 * the camera link (address, DHCP, power) and the control socket.
 *
 * A driver only talks to its camera. Its session() connects, opens the
 * streams it has, publishes frames until the camera fails or the core asks
 * it to stop, and returns. The core calls it again after a pause. Adding a
 * camera type = one rcd_drv_<type>.c plus one entry in the driver table.
 */

#ifndef RCD_H
#define RCD_H

#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include <rss_common.h>
#include <rss_ipc.h>

#include "rcd_tsmap.h"

#define RCD_MAIN 0
#define RCD_SUB	 1

typedef struct rcd_cam rcd_cam_t;

typedef struct {
	const char *name;
	/* Read driver-specific config keys; allocate c->priv. */
	int (*init)(rcd_cam_t *c, rss_config_t *cfg, const char *section);
	/* One connection: returns <0 on camera failure, 0 when asked to stop. */
	int (*session)(rcd_cam_t *c);
	void (*cleanup)(rcd_cam_t *c);
	/* The camera streams once per power-up (Novatek EthCam): after a
	 * session that streamed, the core power-cycles it before the next. */
	bool one_session_per_boot;
} rcd_driver_t;

typedef struct {
	bool open;
	rss_ring_t *ring;
	int codec; /* RSS_CODEC_H264 / RSS_CODEC_H265 */
	int width, height, fps;
	rcd_tsmap_t tsmap; /* both persist across sessions: the camera's clock */
	rcd_seq_t seq;	   /* runs on, so a reconnect gap counts as loss */
	bool info_set;
	int64_t last_frame_us;
	_Atomic uint32_t frames, keyframes, dropped;
	_Atomic uint64_t bytes;
} rcd_stream_t;

struct rcd_cam {
	int index; /* cameraN */
	char section[24];
	const rcd_driver_t *driver;
	void *priv;

	char ip[INET_ADDRSTRLEN];
	char mac[18];
	int slot;  /* publishes sN_main / sN_sub */
	int codec; /* configured codec */
	bool want_sub;
	bool keyframe_requests;
	int data_mb[2];

	pthread_t thread;
	bool thread_started;
	_Atomic int state;
	_Atomic bool restart;
	_Atomic uint32_t sessions;
	pthread_mutex_t info_lock; /* fw, last_error */
	char fw[64];
	char last_error[96];
	rcd_stream_t st[2];
};

/* ── Core services for drivers ── */

/* Keep going? False once the daemon stops or a restart was asked for. */
bool rcd_should_run(rcd_cam_t *c);

/* Create (first time) or refresh the stream's ring; resets clock mapping. */
int rcd_stream_open(rcd_cam_t *c, int idx, int codec, int width, int height, int fps);

/* Timestamp of a frame: the camera's own microsecond clock, or this
 * sentinel when the camera has none (local arrival time is used). */
#define RCD_TS_LOCAL UINT64_MAX

/* Publish one access unit (Annex-B, parameter sets in front of keyframes). */
void rcd_publish(rcd_cam_t *c, int idx, const rss_iov_t *iov, uint32_t n, uint64_t cam_ts_us,
		 bool key);
/* The camera sent a frame we cannot use (bad checksum, ...). */
void rcd_drop(rcd_cam_t *c, int idx, uint64_t cam_ts_us);

/* A consumer asked this stream for a keyframe (clears the request). */
bool rcd_keyframe_wanted(rcd_cam_t *c, int idx);

/* All streams open and started. */
void rcd_streaming(rcd_cam_t *c);

/* Periodic housekeeping; returns -1 when an open stream stalled. */
int rcd_tick(rcd_cam_t *c);

void rcd_set_fw(rcd_cam_t *c, const char *fw);
void rcd_error(rcd_cam_t *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* ── Shared socket helpers ── */

int rcd_tcp_connect(const char *ip, int port, int timeout_ms);
int rcd_send_all(int fd, const void *data, size_t len, int timeout_ms);

/* ── Drivers ── */

extern const rcd_driver_t rcd_drv_ethcam;

#endif

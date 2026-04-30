/*
 * rsr.h — RSR (Raptor SRT listener) internal state
 */

#ifndef RSR_H
#define RSR_H

#include <rss_ipc.h>
#include <rss_common.h>
#include <rss_ts.h>

#include <stdbool.h>
#include <stdint.h>
#include <srt/srt.h>
#include <netinet/in.h>

#define RSR_MAX_CLIENTS	     8
#define RSR_MAX_STREAMS	     6
#define RSR_SRT_PAYLOAD_SIZE 1316 /* 188 * 7 */

/* ── Stream (one per ring, lazily opened) ── */

typedef struct {
	rss_ring_t *ring;
	const char *name;
	uint64_t read_seq;
	uint64_t last_write_seq;
	int idle_count;
	uint32_t codec;
	uint32_t width;
	uint32_t height;
	int client_count;
	bool active;
} rsr_stream_t;

/* ── Client ── */

typedef struct {
	SRTSOCKET sock;
	struct sockaddr_storage addr;
	rsr_stream_t *stream;
	rss_ts_mux_t ts;
	bool waiting_keyframe;
	uint64_t frames_sent;
	uint64_t bytes_sent;
	int64_t connect_time_us;
} rsr_client_t;

/* ── Server state ── */

typedef struct {
	rss_config_t *cfg;
	const char *config_path;
	volatile sig_atomic_t *running;

	SRTSOCKET listener;
	int srt_eid;
	rsr_client_t clients[RSR_MAX_CLIENTS];
	int client_count;
	int max_clients;

	rsr_stream_t streams[RSR_MAX_STREAMS];
	int default_stream_idx;

	rss_ring_t *audio_ring;
	uint64_t audio_read_seq;
	uint64_t audio_last_ws;
	int audio_idle;
	uint32_t audio_codec;
	uint32_t audio_sample_rate;
	uint8_t audio_ts_type;

	uint8_t *frame_buf;
	uint32_t frame_buf_size;
	uint8_t *ts_buf;
	uint32_t ts_buf_size;

	rss_ctrl_t *ctrl;

	int port;
	int latency_ms;
	char passphrase[80];
	int pbkeylen;
	bool audio_enabled;

	uint64_t total_frames;
	uint64_t total_bytes;
} rsr_state_t;

/* ── SRT functions (rsr_srt.c) ── */

void rsr_client_addr_str(const struct sockaddr_storage *addr, char *buf, size_t buf_size);
int rsr_srt_init(rsr_state_t *st);
void rsr_srt_poll(rsr_state_t *st);
int rsr_srt_send_to_client(rsr_client_t *c, const uint8_t *data, size_t len);
void rsr_remove_client(rsr_state_t *st, int idx);
void rsr_srt_cleanup(rsr_state_t *st);

/* ── Stream helpers ── */

extern const char *const rsr_ring_names[RSR_MAX_STREAMS];

rsr_stream_t *rsr_stream_get_or_open(rsr_state_t *st, const char *name);
void rsr_stream_release(rsr_state_t *st, rsr_stream_t *s);

#endif /* RSR_H */

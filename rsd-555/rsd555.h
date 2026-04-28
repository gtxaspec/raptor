/*
 * rsd555.h -- rsd-555 internal shared state
 *
 * live555-based RTSP server consuming SHM ring buffers.
 * Pure ring consumer — no HAL dependency.
 */

#ifndef RSD555_H
#define RSD555_H

#ifndef __cplusplus
#include <rss_ipc.h>
#include <stdbool.h>
#else
/* C++ mode: forward-declare opaque types and functions to avoid
 * C11 _Atomic in rss_ipc.h (not valid C++ syntax). */
extern "C" {
typedef struct rss_ring rss_ring_t;
typedef struct rss_ctrl rss_ctrl_t;
/* Ring header fields we access — must match rss_ipc.h layout.
 * We only read non-atomic fields (set once by producer at create time). */
typedef struct {
	volatile uint64_t write_seq;
	volatile uint32_t futex_seq;
	uint32_t slot_count;
	uint32_t data_size;
	volatile uint32_t data_head;
	uint32_t stream_id;
	uint32_t codec;
	uint32_t width, height;
	uint32_t fps_num, fps_den;
	uint8_t profile;
	uint8_t level;
} rss_ring_header_t __attribute__((aligned(64)));

rss_ring_t *rss_ring_open(const char *name);
void rss_ring_close(rss_ring_t *ring);
int rss_ring_wait(rss_ring_t *ring, uint32_t timeout_ms);
int rss_ring_read(rss_ring_t *ring, uint64_t *read_seq, uint8_t *dest, uint32_t dest_size,
                  uint32_t *length, void *meta);
const rss_ring_header_t *rss_ring_get_header(rss_ring_t *ring);
uint32_t rss_ring_max_frame_size(rss_ring_t *ring);
void rss_ring_request_idr(rss_ring_t *ring);
int rss_ring_check_idr(rss_ring_t *ring);
/* rss_ring_check_version is static inline in rss_ipc.h — not available here.
 * C++ code that needs it should call rsd555_ring_check_version() instead. */

rss_ctrl_t *rss_ctrl_listen(const char *sock_path);
void rss_ctrl_destroy(rss_ctrl_t *ctrl);
int rss_ctrl_get_fd(rss_ctrl_t *ctrl);
int rss_ctrl_accept_and_handle(rss_ctrl_t *ctrl,
                               int (*handler)(const char *cmd_json, char *resp_buf,
                                              int resp_buf_size, void *userdata),
                               void *userdata);
} /* extern "C" */
#endif

#include <rss_common.h>

#include <pthread.h>
#include <stdint.h>
#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RSD555_MAX_CLIENTS    8
#define RSD555_STREAM_COUNT   6 /* main+sub per sensor, up to 3 sensors */
#define RSD555_IDR_REQ_MIN_US 1000000

/* Audio codec IDs (matches RAD ring codec field) */
#define RSD555_CODEC_PCMU 0
#define RSD555_CODEC_PCMA 8
#define RSD555_CODEC_L16  11
#define RSD555_CODEC_AAC  97
#define RSD555_CODEC_OPUS 111

/* Ring names by stream index (defined in rsd555_reader.c) */
extern const char *const rsd555_ring_names[RSD555_STREAM_COUNT];

/* Shared frame data — one allocation, N references.
 * Freed when refcnt drops to zero. */
typedef struct {
	volatile int refcnt;
	uint32_t len;
	int64_t timestamp_us;
	uint16_t nal_type;
	uint8_t is_key;
	uint8_t data[]; /* payload follows via malloc(sizeof + len) */
} rsd555_shared_frame_t;

/* Per-client queue entry — lightweight reference to shared data. */
typedef struct rsd555_frame {
	rsd555_shared_frame_t *shared;
	struct rsd555_frame *next;
} rsd555_frame_t;

/* Lock-protected frame queue between reader thread and FramedSource */
typedef struct {
	rsd555_frame_t *head;
	rsd555_frame_t *tail;
	int count;
	int max_count;
	pthread_mutex_t lock;
	int event_fd; /* signals live555 scheduler when frame arrives */
} rsd555_frame_queue_t;

int rsd555_queue_init(rsd555_frame_queue_t *q, int max_frames);
void rsd555_queue_destroy(rsd555_frame_queue_t *q);
int rsd555_queue_push_ref(rsd555_frame_queue_t *q, rsd555_shared_frame_t *sf);
rsd555_frame_t *rsd555_queue_pop(rsd555_frame_queue_t *q);
void rsd555_frame_free(rsd555_frame_t *f);

/* Create a shared frame (one copy of data, refcnt=1). */
rsd555_shared_frame_t *rsd555_shared_frame_new(const uint8_t *data, uint32_t len,
					       int64_t timestamp_us, uint16_t nal_type,
					       uint8_t is_key);
void rsd555_shared_frame_ref(rsd555_shared_frame_t *sf);
void rsd555_shared_frame_unref(rsd555_shared_frame_t *sf);

/* Per-stream ring reader context.
 * Reader thread fans out frames to all registered source queues
 * (one per live555 client session). */
#define RSD555_MAX_SOURCES 8

typedef struct rsd555_state rsd555_state_t;

typedef struct {
	rss_ring_t *ring;
	uint64_t read_seq;
	uint8_t *frame_buf;
	uint32_t frame_buf_size;
	int idx;
	const char *ring_name;
	volatile sig_atomic_t *running;
	int64_t last_idr_req_us;
	rsd555_state_t *state;

	/* Cached stream info for SDP generation */
	uint32_t codec;
	uint32_t width;
	uint32_t height;
	uint32_t fps_num;
	uint32_t fps_den;
	uint8_t profile;
	uint8_t level;

	/* Cached VPS/SPS/PPS — written by reader thread, read by live555.
	 * volatile for C++ compatibility; MIPS32 aligned 16-bit stores are atomic. */
	uint8_t vps[64];
	uint8_t sps[256];
	uint8_t pps[64];
	volatile uint16_t vps_len;
	volatile uint16_t sps_len;
	volatile uint16_t pps_len;

	/* Per-client source queues — reader pushes to all, each source pops its own */
	rsd555_frame_queue_t *sources[RSD555_MAX_SOURCES];
	int source_count;
	pthread_mutex_t sources_lock;
} rsd555_video_ctx_t;

typedef struct {
	rss_ring_t *ring;
	uint64_t read_seq;
	volatile sig_atomic_t *running;
	rsd555_state_t *state;

	uint32_t codec;
	uint32_t sample_rate;

	rsd555_frame_queue_t *sources[RSD555_MAX_SOURCES];
	int source_count;
	pthread_mutex_t sources_lock;
} rsd555_audio_ctx_t;

/* Source registration — called by FramedSource constructor/destructor */
int rsd555_video_add_source(rsd555_video_ctx_t *ctx, rsd555_frame_queue_t *q);
void rsd555_video_remove_source(rsd555_video_ctx_t *ctx, rsd555_frame_queue_t *q);
int rsd555_audio_add_source(rsd555_audio_ctx_t *ctx, rsd555_frame_queue_t *q);
void rsd555_audio_remove_source(rsd555_audio_ctx_t *ctx, rsd555_frame_queue_t *q);

/* Server state */
struct rsd555_state {
	rss_config_t *cfg;
	const char *config_path;
	rss_ctrl_t *ctrl;
	volatile sig_atomic_t *running;

	int port;
	int max_clients;
	volatile int active_clients;
	int session_timeout;
	char username[128];
	char password[128];
	char session_name[64];
	char session_info[128];
	char endpoints[RSD555_STREAM_COUNT][64];
	char stream_names[RSD555_STREAM_COUNT][64];
	bool video_added[RSD555_STREAM_COUNT];
	bool audio_added;

	rsd555_video_ctx_t video[RSD555_STREAM_COUNT];
	rsd555_audio_ctx_t audio;
	bool has_audio;

	pthread_t video_tids[RSD555_STREAM_COUNT];
	bool video_started[RSD555_STREAM_COUNT];
	pthread_t audio_tid;
	bool audio_started;
};

/* rsd555_reader.c — ring reader threads */
void *rsd555_video_reader_thread(void *arg);
void *rsd555_audio_reader_thread(void *arg);

/* rsd555_ctrl.c — control socket handler */
int rsd555_ctrl_handler(const char *cmd_json, char *resp_buf, int resp_buf_size, void *userdata);

#ifdef __cplusplus
}
#endif

#endif /* RSD555_H */

/*
 * rwd_sendq.h — bounded per-client video send queue.
 *
 * Decouples the ring reader from the send path. The reader must stay
 * inside the refmode copy budget (encoder buffer pool × frame
 * interval — as little as two frames on high-resolution streams), and
 * a main-stream keyframe costs tens of milliseconds of software SRTP,
 * so sending inline from the reader laps the encoder's buffers and
 * every read after a keyframe fails its generation check. The reader
 * pushes copies here; a per-client thread pays the send cost.
 *
 * Overflow policy: a client that cannot drain at stream rate gets its
 * queue purged and resumes clean at the next keyframe — one slow
 * viewer degrades itself, never the reader or other clients.
 */
#ifndef RWD_SENDQ_H
#define RWD_SENDQ_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#define RWD_SENDQ_SLOTS 32 /* ~1s of 30fps video; purged whole on overflow */

typedef struct {
	uint8_t *data; /* malloc'd copy */
	uint32_t len;
	uint32_t rtp_ts;
	int64_t capture_us;
	int64_t enqueue_us;
	bool is_key;
} rwd_sendq_entry_t;

typedef struct {
	int depth;
	int max_depth;
	uint64_t enqueued;
	uint64_t dequeued;
	uint64_t sent;
	uint64_t send_failures;
	uint64_t drops;
	uint64_t bytes_sent;
	uint32_t last_frame_bytes;
	uint32_t max_frame_bytes;
	int64_t last_queue_us;
	int64_t max_queue_us;
	int64_t last_send_us;
	int64_t max_send_us;
	int64_t last_capture_to_send_us;
	int64_t max_capture_to_send_us;
} rwd_sendq_stats_t;

typedef struct {
	rwd_sendq_entry_t entries[RWD_SENDQ_SLOTS];
	int head; /* next push slot */
	int tail; /* next pop slot */
	int count;
	bool shutdown;
	bool needs_keyframe;
	uint64_t drops; /* frames purged by overflow */
	int max_depth;
	uint64_t enqueued;
	uint64_t dequeued;
	uint64_t sent;
	uint64_t send_failures;
	uint64_t bytes_sent;
	uint32_t last_frame_bytes;
	uint32_t max_frame_bytes;
	int64_t last_queue_us;
	int64_t max_queue_us;
	int64_t last_send_us;
	int64_t max_send_us;
	int64_t last_capture_to_send_us;
	int64_t max_capture_to_send_us;
	pthread_mutex_t lock;
	pthread_cond_t cond;
} rwd_sendq_t;

void rwd_sendq_init(rwd_sendq_t *q);

/* Copy a frame in. Returns 0 on success; 1 while recovery requires a
 * keyframe or when the queue was purged; -1 on allocation/shutdown.
 * The caller should re-arm waiting-for-keyframe and request an IDR for
 * either nonzero result. */
int rwd_sendq_push(rwd_sendq_t *q, const uint8_t *data, uint32_t len, uint32_t rtp_ts,
		   int64_t capture_us, int64_t enqueue_us, bool is_key);

/* Blocking pop. Returns false when the queue is shut down; entries
 * still queued at shutdown are freed by rwd_sendq_destroy, not
 * delivered. Caller frees out->data after sending. */
bool rwd_sendq_pop(rwd_sendq_t *q, rwd_sendq_entry_t *out);

/* Record one synchronous packetize/SRTP/send attempt. Times are measured by
 * the send thread, while the queue lock keeps 64-bit counters coherent on
 * 32-bit cameras for control-socket readers. */
void rwd_sendq_note_send(rwd_sendq_t *q, const rwd_sendq_entry_t *entry,
			 int64_t send_start_us, int64_t send_end_us, bool success);

void rwd_sendq_get_stats(rwd_sendq_t *q, rwd_sendq_stats_t *stats);

/* Purge frames queued behind a failed access unit and refuse inter
 * frames until the reader supplies a keyframe. Safe from the send
 * thread while the ring reader is pushing. */
void rwd_sendq_fail(rwd_sendq_t *q);

/* Wake the popper and make it exit; push becomes a no-op. */
void rwd_sendq_shutdown(rwd_sendq_t *q);

/* Free remaining entries and the lock/cond. Only after the send
 * thread is joined. */
void rwd_sendq_destroy(rwd_sendq_t *q);

#endif

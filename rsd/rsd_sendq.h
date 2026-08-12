/*
 * rsd_sendq.h -- per-client bounded send queue (self-contained)
 *
 * No daemon headers on purpose: the unit suite compiles rsd_sendq.c
 * against this header alone.
 */

#ifndef RSD_SENDQ_H
#define RSD_SENDQ_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

/* Per-client send queue — decouples ring reader from network I/O.
 * Every entry holds a malloc'd copy of the frame payload so the
 * reader can overwrite frame_buf with the next ring frame without
 * waiting for any send thread to finish. The memcpy cost is small
 * next to the send-latency hit we'd otherwise take from a barrier
 * wait, especially on slow single-core SoCs. */
#define RSD_SENDQ_SLOTS	  32
#define RSD_FRAME_VIDEO	  0
#define RSD_FRAME_AUDIO	  1
#define RSD_SENDQ_OK	  0
#define RSD_SENDQ_DROPPED 1

typedef struct {
	const uint8_t *data; /* malloc'd copy or rmem pointer (zerocopy) */
	uint32_t len;
	uint32_t rtp_ts;
	uint8_t type;	  /* RSD_FRAME_VIDEO or RSD_FRAME_AUDIO */
	uint32_t codec;	  /* audio codec (RSD_FRAME_AUDIO only) */
	bool zerocopy;	  /* true = rmem pointer, don't free */
	uint8_t buf_idx;  /* refmode: encoder buffer index */
	uint32_t buf_gen; /* refmode: generation at peek time */
} rsd_sendq_entry_t;

typedef struct {
	rsd_sendq_entry_t entries[RSD_SENDQ_SLOTS];
	int head;
	int tail;
	int count;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	bool shutdown;
	/*
	 * Discard accounting. One queue carries both streams, audio pushes at
	 * 50/s against video's 30/s, and a single video entry can hold the send
	 * thread for the length of an IDR's worth of blocking writes -- so
	 * RSD_SENDQ_SLOTS is about 400ms of a 25fps video plus AAC stream. This
	 * absorbs ordinary Wi-Fi scheduling stalls without growing the queue far
	 * enough to hide a genuinely slow client. Without these counters an
	 * overflow is indistinguishable from a capture fault, which is exactly
	 * the confusion that cost a round of board testing on the
	 * audio dropouts.
	 */
	uint32_t drop_audio; /* audio entries discarded to overflow */
	uint32_t drop_video; /* video entries discarded to overflow */
	uint32_t overflows;  /* times the queue was full on push */
} rsd_sendq_t;

int rsd_sendq_init(rsd_sendq_t *q);
void rsd_sendq_destroy(rsd_sendq_t *q);
void rsd_sendq_release_entry(rsd_sendq_entry_t *e);
/* Caller holds q->lock. */
bool rsd_sendq_take_audio_locked(rsd_sendq_t *q, rsd_sendq_entry_t *out);
int rsd_sendq_push_video(rsd_sendq_t *q, const uint8_t *data, uint32_t len, uint32_t rtp_ts,
			 const uint8_t *sei, uint32_t sei_len, bool is_h265);
int rsd_sendq_push_audio(rsd_sendq_t *q, uint32_t codec, const uint8_t *data, uint32_t len,
			 uint32_t rtp_ts);

#endif /* RSD_SENDQ_H */

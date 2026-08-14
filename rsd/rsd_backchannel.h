/*
 * rsd_backchannel.h -- backchannel (client → server) audio decode
 *
 * Compy-free on purpose: the unit-test binary compiles this module
 * standalone, so everything RTSP-flavored (SDP, SETUP, the
 * Compy_AudioReceiver glue) stays in rsd_session.c and only the raw
 * (payload type, payload) → speaker-ring PCM path lives here.
 */

#ifndef RSD_BACKCHANNEL_H
#define RSD_BACKCHANNEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <rss_ipc.h>

/*
 * Dynamic payload types offered on the backchannel m-line. Scoped to
 * that m-line (RFC 8866 §5.14), but kept distinct from the forward
 * track's 97/98/111 so a PT in a log line names one codec, not two.
 * PCMU (0) and PCMA (8) ride their RFC 3551 static assignments.
 */
#define RSD_BC_PT_OPUS 112 /* opus/48000/2 (RFC 7587) */
#define RSD_BC_PT_AAC  113 /* MPEG4-GENERIC/16000/1 AAC-hbr (RFC 3640) */
#define RSD_BC_PT_L16  114 /* L16/16000/1 (RFC 3551 §4.5.11) */

/* The speaker ring is PCM16 mono at this rate; every decoder below
 * converges on it so rad's AO path never resamples. */
#define RSD_BC_RING_RATE 16000

/* Per-client decoder state. Plain struct, embed and init/deinit. */
typedef struct {
	uint8_t last_pt; /* logs codec switches; valid when have_pt */
	bool have_pt;
	uint32_t unknown_pt_count;
	int64_t last_warn_us; /* shared rate limit for drop/error warns */
	/* Lazily created codec decoders. void* keeps this header free of
	 * codec includes; the members exist in every build so the struct
	 * layout never depends on codec flags. */
	void *opus; /* OpusDecoder*, RAPTOR_OPUS builds */
	bool opus_dead;
} rsd_bc_dec_t;

void rsd_bc_dec_init(rsd_bc_dec_t *d);
void rsd_bc_dec_deinit(rsd_bc_dec_t *d);

/*
 * Decode one RTP payload and publish PCM16/16 kHz mono into the
 * speaker ring. Payload types outside the offered set are counted and
 * dropped (never published raw: rad plays this ring as PCM).
 */
void rsd_bc_handle(rsd_bc_dec_t *d, rss_ring_t *ring, uint8_t pt, const uint8_t *payload,
		   size_t len);

/* G.711 expansion primitives, exported for the unit tests. */
int16_t rsd_bc_ulaw_decode(uint8_t ulaw);
int16_t rsd_bc_alaw_decode(uint8_t alaw);

#endif /* RSD_BACKCHANNEL_H */

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

/* Codec selection bits for [rtsp] backchannel_codecs. */
#define RSD_BC_CODEC_PCMU (1u << 0)
#define RSD_BC_CODEC_PCMA (1u << 1)
#define RSD_BC_CODEC_OPUS (1u << 2)
#define RSD_BC_CODEC_AAC  (1u << 3)
#define RSD_BC_CODEC_L16  (1u << 4)

/* What this build can decode. */
uint32_t rsd_bc_codecs_available(void);

/*
 * Parse a comma/space-separated codec list ("pcmu,opus", case
 * folded). Unrecognized tokens are skipped; the first one is copied
 * to `unknown` (empty string when all parsed) so boot-time validation
 * can name it once. Returns the requested mask, 0 for an empty list.
 */
uint32_t rsd_bc_codecs_parse(const char *list, char *unknown, size_t unknown_cap);

/*
 * The mask that actually governs offer and dispatch: an empty request
 * means everything, and the result is clamped to what the build can
 * decode -- never 0, so a config of nothing-usable degrades to the
 * full offer rather than a backchannel that negotiates and then eats
 * every packet.
 */
uint32_t rsd_bc_codecs_effective(uint32_t requested);

/* "0 8 112 113 114" for the m-line, subset per mask, canonical order.
 * Returns the number of payload types written. */
int rsd_bc_offer_pts(uint32_t mask, char *buf, size_t cap);

/* "pcmu,pcma,..." for logs and status, subset per mask. */
const char *rsd_bc_codec_names(uint32_t mask, char *buf, size_t cap);

/* The governing mask from [rtsp] backchannel_codecs, read fresh so a
 * live set-backchannel-codecs applies to the next session. */
struct rss_config;
uint32_t rsd_bc_enabled_mask(struct rss_config *cfg);

/* Codec name for a backchannel payload type ("?" when unmapped). */
const char *rsd_bc_pt_name(uint8_t pt);

/* One AAC-LC access unit is bounded by the 6144-bit-per-channel bit
 * reservoir; mono makes that 768 bytes. Doubled for headroom. */
#define RSD_BC_AAC_MAX_AU  1536
#define RSD_BC_AAC_MAX_AUS 16

/* Per-client decoder state. Plain struct, embed and init/deinit. */
typedef struct {
	uint32_t enabled; /* RSD_BC_CODEC_* mask this session offered */
	uint8_t last_pt;  /* logs codec switches; valid when have_pt */
	bool have_pt;
	uint32_t unknown_pt_count;
	int64_t last_warn_us; /* shared rate limit for drop/error warns */
	/* Lazily created codec decoders. void* keeps this header free of
	 * codec includes; the members exist in every build so the struct
	 * layout never depends on codec flags. */
	void *opus; /* OpusDecoder*, RAPTOR_OPUS builds */
	bool opus_dead;
	void *aac; /* HAACDecoder, RAPTOR_AAC builds */
	bool aac_dead;
	/* RFC 3640 fragmented-AU reassembly: fragments carry the full AU
	 * size in every piece and share one RTP timestamp. */
	uint32_t aac_frag_ts;
	size_t aac_frag_expect; /* 0 = no fragment pending */
	size_t aac_frag_len;
	uint8_t aac_frag[RSD_BC_AAC_MAX_AU];
} rsd_bc_dec_t;

/* An access unit inside one RTP payload (parse result). */
typedef struct {
	const uint8_t *ptr;
	size_t len;
} rsd_bc_au_t;

/*
 * Parse an RFC 3640 AAC-hbr payload (sizeLength=13, indexLength=3,
 * indexDeltaLength=3, no interleaving -- exactly what the SDP offers).
 *
 * Returns the number of complete AUs written to aus, 0 for a fragment
 * (aus[0] holds the piece, *frag_total the full AU size), or -1 for a
 * payload that does not follow the offered format.
 */
int rsd_bc_aac_parse(const uint8_t *p, size_t len, rsd_bc_au_t *aus, int max_aus,
		     size_t *frag_total);

void rsd_bc_dec_init(rsd_bc_dec_t *d, uint32_t enabled_mask);
void rsd_bc_dec_deinit(rsd_bc_dec_t *d);

/*
 * Decode one RTP payload and publish PCM16/16 kHz mono into the
 * speaker ring. Payload types outside the offered set are counted and
 * dropped (never published raw: rad plays this ring as PCM). rtp_ts
 * only matters to AAC, whose fragment reassembly keys on it.
 */
void rsd_bc_handle(rsd_bc_dec_t *d, rss_ring_t *ring, uint8_t pt, uint32_t rtp_ts,
		   const uint8_t *payload, size_t len);

/* G.711 expansion primitives, exported for the unit tests. */
int16_t rsd_bc_ulaw_decode(uint8_t ulaw);
int16_t rsd_bc_alaw_decode(uint8_t alaw);

#endif /* RSD_BACKCHANNEL_H */

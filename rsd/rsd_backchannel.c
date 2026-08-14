/*
 * rsd_backchannel.c -- backchannel (client → server) audio decode
 *
 * Every codec lands as PCM16 mono 16 kHz in the speaker ring. G.711
 * arrives at 8 kHz and is upsampled by sample doubling; L16 is offered
 * at the ring rate so it only needs the RFC 3551 network-byte-order
 * swap.
 */

#include <rss_common.h>

#ifdef RAPTOR_OPUS
#include <opus/opus.h>
#endif

#include "rsd_backchannel.h"

/* G.711 payloads run at 8 kHz; cap one packet at 60 ms (480 samples)
 * so the doubled output stays a small stack buffer. */
#define BC_G711_MAX_SAMPLES 480

/* L16 cap per packet, in samples. Real senders use 20 ms ptime (320
 * samples); the cap only bounds the stack against a hostile 64 KB
 * interleaved frame. */
#define BC_L16_MAX_SAMPLES	    4096

#define BC_UNKNOWN_WARN_INTERVAL_US 5000000

int16_t rsd_bc_ulaw_decode(uint8_t ulaw)
{
	ulaw = ~ulaw;
	int sign = (ulaw & 0x80);
	int exponent = (ulaw >> 4) & 0x07;
	int mantissa = ulaw & 0x0f;
	int magnitude = ((mantissa << 3) + 0x84) << exponent;
	magnitude -= 0x84;
	return (int16_t)(sign ? -magnitude : magnitude);
}

int16_t rsd_bc_alaw_decode(uint8_t alaw)
{
	alaw ^= 0x55;
	int exponent = (alaw >> 4) & 0x07;
	int magnitude = (alaw & 0x0f) << 4;
	if (exponent > 0)
		magnitude = (magnitude + 0x108) << (exponent - 1);
	else
		magnitude += 8;
	/* A-law transmits 1 in the sign bit for POSITIVE samples --
	 * inverted from mu-law's convention. */
	return (int16_t)((alaw & 0x80) ? magnitude : -magnitude);
}

void rsd_bc_dec_init(rsd_bc_dec_t *d)
{
	*d = (rsd_bc_dec_t){0};
}

void rsd_bc_dec_deinit(rsd_bc_dec_t *d)
{
#ifdef RAPTOR_OPUS
	if (d->opus) {
		opus_decoder_destroy((OpusDecoder *)d->opus);
		d->opus = NULL;
	}
#endif
	(void)d;
}

static const char *bc_pt_name(uint8_t pt)
{
	switch (pt) {
	case 0:
		return "PCMU";
	case 8:
		return "PCMA";
	case RSD_BC_PT_OPUS:
		return "opus";
	case RSD_BC_PT_AAC:
		return "AAC";
	case RSD_BC_PT_L16:
		return "L16";
	default:
		return "?";
	}
}

static void bc_note_codec(rsd_bc_dec_t *d, uint8_t pt)
{
	if (d->have_pt && d->last_pt == pt)
		return;
	/* The client picks its codec by just sending it (RTSP has no SDP
	 * answer), so this line is the only place the choice is visible. */
	RSS_INFO("backchannel: receiving %s (payload type %u)", bc_pt_name(pt), pt);
	d->last_pt = pt;
	d->have_pt = true;
}

/* Decode one G.711 payload (mu- or A-law) and double 8 kHz → 16 kHz. */
static void bc_handle_g711(rsd_bc_dec_t *d, rss_ring_t *ring, uint8_t pt, const uint8_t *payload,
			   size_t len)
{
	int16_t pcm[BC_G711_MAX_SAMPLES * 2];
	int n = (int)len;
	if (n > BC_G711_MAX_SAMPLES)
		n = BC_G711_MAX_SAMPLES;
	for (int i = 0; i < n; i++) {
		int16_t s =
			(pt == 8) ? rsd_bc_alaw_decode(payload[i]) : rsd_bc_ulaw_decode(payload[i]);
		pcm[i * 2] = s;
		pcm[i * 2 + 1] = s;
	}
	bc_note_codec(d, pt);
	rss_ring_publish(ring, (const uint8_t *)pcm, (uint32_t)(n * 4), rss_timestamp_us(), 0, 0);
}

/* L16/16000/1: network byte order to host, already at ring rate. */
static void bc_handle_l16(rsd_bc_dec_t *d, rss_ring_t *ring, const uint8_t *payload, size_t len)
{
	int16_t pcm[BC_L16_MAX_SAMPLES];
	int n = (int)(len / 2);
	if (n > BC_L16_MAX_SAMPLES)
		n = BC_L16_MAX_SAMPLES;
	if (n == 0)
		return;
	for (int i = 0; i < n; i++)
		pcm[i] = (int16_t)(((uint16_t)payload[i * 2] << 8) | payload[i * 2 + 1]);
	bc_note_codec(d, RSD_BC_PT_L16);
	rss_ring_publish(ring, (const uint8_t *)pcm, (uint32_t)(n * 2), rss_timestamp_us(), 0, 0);
}

static bool bc_warn_due(rsd_bc_dec_t *d)
{
	int64_t now = rss_timestamp_us();
	if (now - d->last_warn_us < BC_UNKNOWN_WARN_INTERVAL_US)
		return false;
	d->last_warn_us = now;
	return true;
}

static void bc_note_unknown(rsd_bc_dec_t *d, uint8_t pt)
{
	d->unknown_pt_count++;
	if (bc_warn_due(d))
		RSS_WARN("backchannel: dropping payload type %u (not offered, %u total)", pt,
			 d->unknown_pt_count);
}

#ifdef RAPTOR_OPUS
/* RFC 7587 §7: a conformant decoder accepts any packet regardless of
 * coded rate or channels; created at the ring rate, libopus hands back
 * 16 kHz mono directly. One packet may carry up to 120 ms. */
#define BC_OPUS_MAX_SAMPLES (RSD_BC_RING_RATE / 1000 * 120)

static void bc_handle_opus(rsd_bc_dec_t *d, rss_ring_t *ring, const uint8_t *payload, size_t len)
{
	if (d->opus_dead)
		return;
	if (!d->opus) {
		int err = 0;
		d->opus = opus_decoder_create(RSD_BC_RING_RATE, 1, &err);
		if (!d->opus) {
			/* Allocation failure will not heal mid-session;
			 * warn once and stop trying. */
			RSS_WARN("backchannel: opus decoder init failed (%d)", err);
			d->opus_dead = true;
			return;
		}
	}
	int16_t pcm[BC_OPUS_MAX_SAMPLES];
	int n = opus_decode((OpusDecoder *)d->opus, payload, (opus_int32)len, pcm,
			    BC_OPUS_MAX_SAMPLES, 0);
	if (n <= 0) {
		if (bc_warn_due(d))
			RSS_WARN("backchannel: opus decode failed (%d)", n);
		return;
	}
	bc_note_codec(d, RSD_BC_PT_OPUS);
	rss_ring_publish(ring, (const uint8_t *)pcm, (uint32_t)(n * 2), rss_timestamp_us(), 0, 0);
}
#endif /* RAPTOR_OPUS */

void rsd_bc_handle(rsd_bc_dec_t *d, rss_ring_t *ring, uint8_t pt, const uint8_t *payload,
		   size_t len)
{
	if (!ring || len == 0)
		return;

	switch (pt) {
	case 0:
	case 8:
		bc_handle_g711(d, ring, pt, payload, len);
		return;
	case RSD_BC_PT_L16:
		bc_handle_l16(d, ring, payload, len);
		return;
#ifdef RAPTOR_OPUS
	case RSD_BC_PT_OPUS:
		bc_handle_opus(d, ring, payload, len);
		return;
#endif
	default:
		bc_note_unknown(d, pt);
		return;
	}
}

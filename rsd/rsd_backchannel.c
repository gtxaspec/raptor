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
#ifdef RAPTOR_AAC
#include <aacdec.h>
#endif

#include <string.h>

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
#ifdef RAPTOR_AAC
	if (d->aac) {
		AACFreeDecoder((HAACDecoder)d->aac);
		d->aac = NULL;
	}
#endif
	(void)d;
}

/*
 * RFC 3640 AAC-hbr: a 16-bit AU-headers-length (in bits) fronts a run
 * of 16-bit AU headers -- size(13) | index or indexDelta(3) -- then
 * the AU data back to back. The offer carries no `interleaving', so a
 * nonzero index/indexDelta is a contract violation, and a fragmented
 * AU must be alone in its packet with the header naming the FULL size.
 */
int rsd_bc_aac_parse(const uint8_t *p, size_t len, rsd_bc_au_t *aus, int max_aus,
		     size_t *frag_total)
{
	*frag_total = 0;
	if (len < 4)
		return -1;

	uint32_t hdr_bits = ((uint32_t)p[0] << 8) | p[1];
	if (hdr_bits == 0 || hdr_bits % 16 != 0)
		return -1;
	uint32_t n_aus = hdr_bits / 16;
	size_t hdr_bytes = 2 + hdr_bits / 8;
	if (n_aus > (uint32_t)max_aus || hdr_bytes >= len)
		return -1;

	const uint8_t *data = p + hdr_bytes;
	size_t data_len = len - hdr_bytes;
	size_t off = 0;

	for (uint32_t i = 0; i < n_aus; i++) {
		uint16_t h = (uint16_t)(((uint16_t)p[2 + i * 2] << 8) | p[3 + i * 2]);
		size_t au_size = h >> 3;
		if ((h & 0x7) != 0 || au_size == 0)
			return -1;
		if (off + au_size > data_len) {
			/* Short data is legal only as a lone fragmented AU. */
			if (n_aus == 1 && off == 0) {
				aus[0].ptr = data;
				aus[0].len = data_len;
				*frag_total = au_size;
				return 0;
			}
			return -1;
		}
		aus[i].ptr = data + off;
		aus[i].len = au_size;
		off += au_size;
	}
	if (off != data_len)
		return -1; /* trailing bytes the headers never named */
	return (int)n_aus;
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

#ifdef RAPTOR_AAC
static void bc_aac_frag_reset(rsd_bc_dec_t *d)
{
	d->aac_frag_expect = 0;
	d->aac_frag_len = 0;
}

static void bc_aac_decode_au(rsd_bc_dec_t *d, rss_ring_t *ring, const uint8_t *au, size_t au_len)
{
	if (d->aac_dead)
		return;
	if (!d->aac) {
		d->aac = AACInitDecoder();
		if (!d->aac) {
			RSS_WARN("backchannel: AAC decoder init failed");
			d->aac_dead = true;
			return;
		}
		/* RFC 3640 AUs are raw data blocks; the stream properties
		 * come from the fmtp config WE advertised (AAC-LC, ring
		 * rate, mono), so nonconforming input decodes as the error
		 * it is rather than being guessed at. */
		AACFrameInfo fi = {0};
		fi.nChans = 1;
		fi.sampRateCore = RSD_BC_RING_RATE;
		fi.profile = AAC_PROFILE_LC;
		int err = AACSetRawBlockParams((HAACDecoder)d->aac, 0, &fi);
		if (err != 0) {
			RSS_WARN("backchannel: AAC raw params rejected (%d)", err);
			AACFreeDecoder((HAACDecoder)d->aac);
			d->aac = NULL;
			d->aac_dead = true;
			return;
		}
	}

	/* Helix is built with SBR, and raw-block params do not stop a
	 * hostile AU from carrying a CPE or an SBR extension: it decodes
	 * what the bitstream says, up to 2048 samples per channel. The
	 * buffer covers that worst case; conforming input (LC mono per
	 * our fmtp) uses a quarter of it. */
	int16_t pcm[AAC_MAX_NSAMPS * AAC_MAX_NCHANS * 2];
	unsigned char *inp = (unsigned char *)au;
	int left = (int)au_len;
	int err = AACDecode((HAACDecoder)d->aac, &inp, &left, pcm);
	if (err != 0) {
		if (bc_warn_due(d))
			RSS_WARN("backchannel: AAC decode failed (%d)", err);
		return;
	}
	AACFrameInfo info;
	AACGetLastFrameInfo((HAACDecoder)d->aac, &info);
	int samples = info.outputSamps;
	if (info.nChans == 2) {
		samples /= 2;
		for (int i = 0; i < samples; i++)
			pcm[i] = (int16_t)((pcm[i * 2] + pcm[i * 2 + 1]) / 2);
	}
	if (samples <= 0)
		return;
	bc_note_codec(d, RSD_BC_PT_AAC);
	rss_ring_publish(ring, (const uint8_t *)pcm, (uint32_t)(samples * 2), rss_timestamp_us(), 0,
			 0);
}

static void bc_handle_aac(rsd_bc_dec_t *d, rss_ring_t *ring, uint32_t rtp_ts,
			  const uint8_t *payload, size_t len)
{
	rsd_bc_au_t aus[RSD_BC_AAC_MAX_AUS];
	size_t frag_total = 0;
	int n = rsd_bc_aac_parse(payload, len, aus, RSD_BC_AAC_MAX_AUS, &frag_total);
	if (n < 0) {
		bc_aac_frag_reset(d);
		if (bc_warn_due(d))
			RSS_WARN("backchannel: malformed AAC-hbr payload (%zu bytes)", len);
		return;
	}
	if (n == 0) {
		/* Fragment piece. Every piece repeats the full AU size and
		 * shares the AU's timestamp; a mismatch means the previous
		 * AU died in transit, so start over on this one. */
		if (frag_total > RSD_BC_AAC_MAX_AU) {
			bc_aac_frag_reset(d);
			if (bc_warn_due(d))
				RSS_WARN("backchannel: AAC AU of %zu bytes exceeds the %d cap",
					 frag_total, RSD_BC_AAC_MAX_AU);
			return;
		}
		if (d->aac_frag_expect != frag_total || d->aac_frag_ts != rtp_ts)
			bc_aac_frag_reset(d);
		if (d->aac_frag_expect == 0) {
			d->aac_frag_expect = frag_total;
			d->aac_frag_ts = rtp_ts;
		}
		if (d->aac_frag_len + aus[0].len > d->aac_frag_expect) {
			bc_aac_frag_reset(d);
			return;
		}
		memcpy(d->aac_frag + d->aac_frag_len, aus[0].ptr, aus[0].len);
		d->aac_frag_len += aus[0].len;
		if (d->aac_frag_len == d->aac_frag_expect) {
			bc_aac_decode_au(d, ring, d->aac_frag, d->aac_frag_len);
			bc_aac_frag_reset(d);
		}
		return;
	}
	bc_aac_frag_reset(d); /* a complete packet supersedes a half AU */
	for (int i = 0; i < n; i++)
		bc_aac_decode_au(d, ring, aus[i].ptr, aus[i].len);
}
#endif /* RAPTOR_AAC */

void rsd_bc_handle(rsd_bc_dec_t *d, rss_ring_t *ring, uint8_t pt, uint32_t rtp_ts,
		   const uint8_t *payload, size_t len)
{
	(void)rtp_ts;
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
#ifdef RAPTOR_AAC
	case RSD_BC_PT_AAC:
		bc_handle_aac(d, ring, rtp_ts, payload, len);
		return;
#endif
	default:
		bc_note_unknown(d, pt);
		return;
	}
}

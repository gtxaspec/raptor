/*
 * rhd_audio.c -- HTTP audio streaming
 *
 * Reads from the audio ring and streams to HTTP clients.
 * Codec framing:
 *   PCMU/PCMA → decoded to PCM16 LE with WAV header
 *   L16       → WAV header + raw PCM16
 *   AAC       → ADTS headers prepended to each frame
 *   Opus      → Ogg/Opus container (OpusHead + OpusTags + data pages)
 */

#include <string.h>
#include <arpa/inet.h>

#include "rhd.h"

/* Use codec IDs from rhd.h (RHD_CODEC_*) */

/* ── G.711 decode tables ── */

static int16_t ulaw_to_pcm16(uint8_t ulaw)
{
	ulaw = ~ulaw;
	int sign = (ulaw & 0x80);
	int exponent = (ulaw >> 4) & 0x07;
	int mantissa = ulaw & 0x0F;
	int magnitude = ((mantissa << 3) + 0x84) << exponent;
	magnitude -= 0x84;
	return (int16_t)(sign ? -magnitude : magnitude);
}

static int16_t alaw_to_pcm16(uint8_t alaw)
{
	alaw ^= 0x55;
	int sign = (alaw & 0x80);
	int exponent = (alaw >> 4) & 0x07;
	int mantissa = alaw & 0x0F;
	int magnitude;
	if (exponent > 0)
		magnitude = ((mantissa << 4) | 0x108) << (exponent - 1);
	else
		magnitude = (mantissa << 4) | 0x08;
	return (int16_t)(sign ? -magnitude : magnitude);
}

/* ── WAV header (streaming: length = 0xFFFFFFFF) ── */

_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
	       "WAV header uses native memcpy and assumes little-endian host");

static int build_wav_header(uint8_t *buf, int sample_rate)
{
	int byte_rate = sample_rate * 2; /* 16-bit mono */
	memcpy(buf, "RIFF", 4);
	uint32_t v = 0xFFFFFFFF; /* unknown length for streaming */
	memcpy(buf + 4, &v, 4);
	memcpy(buf + 8, "WAVEfmt ", 8);
	v = 16;
	memcpy(buf + 16, &v, 4); /* chunk size */
	uint16_t s = 1;
	memcpy(buf + 20, &s, 2); /* PCM format */
	s = 1;
	memcpy(buf + 22, &s, 2); /* mono */
	v = (uint32_t)sample_rate;
	memcpy(buf + 24, &v, 4); /* sample rate */
	v = (uint32_t)byte_rate;
	memcpy(buf + 28, &v, 4); /* byte rate */
	s = 2;
	memcpy(buf + 32, &s, 2); /* block align */
	s = 16;
	memcpy(buf + 34, &s, 2); /* bits per sample */
	memcpy(buf + 36, "data", 4);
	v = 0xFFFFFFFF;
	memcpy(buf + 40, &v, 4); /* data chunk size (streaming) */
	return 44;
}

/* ── ADTS header for AAC frames ── */

static int build_adts_header(uint8_t *buf, int sample_rate, int frame_len)
{
	/* Find sample rate index */
	static const int sr_table[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
				       22050, 16000, 12000, 11025, 8000,  7350};
	int sr_idx = 4; /* default 44100 */
	for (int i = 0; i < 13; i++) {
		if (sr_table[i] == sample_rate) {
			sr_idx = i;
			break;
		}
	}

	int total = frame_len + 7; /* ADTS header is 7 bytes */
	if (total > 8191)
		total = 8191; /* ADTS 13-bit length field max */
	buf[0] = 0xFF;
	buf[1] = 0xF1;						/* ADTS, MPEG-4, no CRC */
	buf[2] = (uint8_t)(((2 - 1) << 6) | (sr_idx << 2) | 0); /* AAC-LC, sr_idx, private=0 */
	buf[3] = (uint8_t)((1 << 6) | ((total >> 11) & 0x03));	/* channels=1, frame_len high */
	buf[4] = (uint8_t)((total >> 3) & 0xFF);		/* frame_len mid */
	buf[5] = (uint8_t)(((total & 0x07) << 5) | 0x1F);	/* frame_len low, buffer fullness */
	buf[6] = 0xFC;						/* buffer fullness, 1 frame */
	return 7;
}

/* ── Ogg page builder ── */

/* CRC32 lookup table for Ogg (polynomial 0x04C11DB7) */
static uint32_t ogg_crc_table[256];
static bool ogg_crc_init_done;

static void ogg_crc_init(void)
{
	if (ogg_crc_init_done)
		return;
	for (int i = 0; i < 256; i++) {
		uint32_t r = (uint32_t)i << 24;
		for (int j = 0; j < 8; j++)
			r = (r << 1) ^ ((r & 0x80000000) ? 0x04C11DB7 : 0);
		ogg_crc_table[i] = r;
	}
	ogg_crc_init_done = true;
}

static uint32_t ogg_crc(const uint8_t *data, int len)
{
	uint32_t crc = 0;
	for (int i = 0; i < len; i++)
		crc = (crc << 8) ^ ogg_crc_table[((crc >> 24) & 0xFF) ^ data[i]];
	return crc;
}

/*
 * Build an Ogg page. Returns total page size written to buf.
 * header_type: 0x02 = BOS (beginning of stream), 0x00 = normal, 0x04 = EOS
 * Note: Ogg integers are little-endian; relies on _Static_assert above.
 */
static int ogg_build_page(uint8_t *buf, int buf_size, uint8_t header_type, uint64_t granule,
			  uint32_t serial, uint32_t page_seq, const uint8_t *data, int data_len)
{
	/* Segment table: each segment max 255 bytes; data_len/255 full segments + remainder */
	int n_segments = data_len / 255 + 1;
	int hdr_size = 27 + n_segments;
	if (hdr_size + data_len > buf_size)
		return -1;

	memcpy(buf, "OggS", 4);
	buf[4] = 0; /* version */
	buf[5] = header_type;
	memcpy(buf + 6, &granule, 8); /* granule position (LE) */
	memcpy(buf + 14, &serial, 4);
	memcpy(buf + 18, &page_seq, 4);
	memset(buf + 22, 0, 4); /* CRC placeholder */
	buf[26] = (uint8_t)n_segments;

	/* Segment table */
	int remaining = data_len;
	for (int i = 0; i < n_segments; i++) {
		if (remaining >= 255) {
			buf[27 + i] = 255;
			remaining -= 255;
		} else {
			buf[27 + i] = (uint8_t)remaining;
			remaining = 0;
		}
	}

	memcpy(buf + hdr_size, data, data_len);

	/* Compute and fill CRC */
	uint32_t crc = ogg_crc(buf, hdr_size + data_len);
	memcpy(buf + 22, &crc, 4);

	return hdr_size + data_len;
}

/* ── Public API ── */

int rhd_audio_send_header(rhd_client_t *c, int codec, int sample_rate)
{
	if (codec == RHD_CODEC_AAC) {
		/* ADTS stream — send HTTP header only, ADTS per frame */
		char hdr[256];
		int n = snprintf(hdr, sizeof(hdr),
				 "HTTP/1.1 200 OK\r\n"
				 "Content-Type: audio/aac\r\n"
				 "Transfer-Encoding: chunked\r\n"
				 "Cache-Control: no-cache\r\n"
				 "Connection: close\r\n"
				 "\r\n");
		return nb_write_all(c, hdr, n);
	}

	if (codec == RHD_CODEC_OPUS) {
		ogg_crc_init();
		/* Ogg/Opus: HTTP header + OpusHead + OpusTags pages */
		char hdr[256];
		int n = snprintf(hdr, sizeof(hdr),
				 "HTTP/1.1 200 OK\r\n"
				 "Content-Type: audio/ogg\r\n"
				 "Transfer-Encoding: chunked\r\n"
				 "Cache-Control: no-cache\r\n"
				 "Connection: close\r\n"
				 "\r\n");
		if (nb_write_all(c, hdr, n) < 0)
			return -1;

		/* OpusHead packet (19 bytes) */
		uint8_t opus_head[19] = {0};
		memcpy(opus_head, "OpusHead", 8);
		opus_head[8] = 1;  /* version */
		opus_head[9] = 1;  /* channels */
		opus_head[10] = 0; /* pre-skip LE (low) */
		opus_head[11] = 0; /* pre-skip LE (high) */
		uint32_t sr = (uint32_t)sample_rate;
		memcpy(opus_head + 12, &sr, 4); /* original sample rate LE */
		opus_head[16] = 0;		/* output gain LE (low) */
		opus_head[17] = 0;		/* output gain LE (high) */
		opus_head[18] = 0;		/* channel map family */

		uint8_t page[256];
		uint32_t serial = 0x52415054; /* "RAPT" */
		int plen = ogg_build_page(page, sizeof(page), 0x02, 0, serial, 0, opus_head, 19);
		if (plen < 0)
			return -1;

		/* Send as chunked */
		char chunk_hdr[16];
		int chn = snprintf(chunk_hdr, sizeof(chunk_hdr), "%x\r\n", plen);
		if (nb_write_all(c, chunk_hdr, chn) < 0)
			return -1;
		if (nb_write_all(c, page, plen) < 0)
			return -1;
		if (nb_write_all(c, "\r\n", 2) < 0)
			return -1;

		/* OpusTags packet: 8 (magic) + 4 (vendor len) + 6 (vendor) + 4 (comment count) = 22
		 */
		uint8_t tags_full[22];
		memcpy(tags_full, "OpusTags", 8);
		uint32_t vlen = 6;
		memcpy(tags_full + 8, &vlen, 4);
		memcpy(tags_full + 12, "Raptor", 6);
		uint32_t zero = 0;
		memcpy(tags_full + 18, &zero, 4); /* 0 comments */
		int tags_len = 22;

		plen = ogg_build_page(page, sizeof(page), 0x00, 0, serial, 1, tags_full, tags_len);
		if (plen < 0)
			return -1;
		chn = snprintf(chunk_hdr, sizeof(chunk_hdr), "%x\r\n", plen);
		if (nb_write_all(c, chunk_hdr, chn) < 0)
			return -1;
		if (nb_write_all(c, page, plen) < 0)
			return -1;
		if (nb_write_all(c, "\r\n", 2) < 0)
			return -1;

		return 0;
	}

	/* PCM/G.711/L16 — WAV stream */
	char hdr[256];
	int n = snprintf(hdr, sizeof(hdr),
			 "HTTP/1.1 200 OK\r\n"
			 "Content-Type: audio/wav\r\n"
			 "Cache-Control: no-cache\r\n"
			 "Connection: close\r\n"
			 "\r\n");
	if (nb_write_all(c, hdr, n) < 0)
		return -1;

	uint8_t wav[44];
	build_wav_header(wav, sample_rate);
	return nb_write_all(c, wav, 44);
}

int rhd_audio_send_frame(rhd_client_t *c, int codec, int sample_rate, const uint8_t *data,
			 uint32_t len, uint32_t page_seq, uint64_t granule)
{
	if (codec == RHD_CODEC_AAC) {
		/* Prepend ADTS header */
		uint8_t adts[7];
		build_adts_header(adts, sample_rate, (int)len);
		int total = 7 + (int)len;
		char chunk_hdr[16];
		int chn = snprintf(chunk_hdr, sizeof(chunk_hdr), "%x\r\n", total);
		if (nb_write_all(c, chunk_hdr, chn) < 0)
			return -1;
		if (nb_write_all(c, adts, 7) < 0)
			return -1;
		if (nb_write_all(c, data, len) < 0)
			return -1;
		return nb_write_all(c, "\r\n", 2);
	}

	if (codec == RHD_CODEC_OPUS) {
		/* Wrap in Ogg page */
		uint8_t page[4096];
		uint32_t serial = 0x52415054;
		int plen = ogg_build_page(page, sizeof(page), 0x00, granule, serial,
					  page_seq + 2, /* +2 for OpusHead and OpusTags pages */
					  data, (int)len);
		if (plen < 0)
			return -1;
		char chunk_hdr[16];
		int chn = snprintf(chunk_hdr, sizeof(chunk_hdr), "%x\r\n", plen);
		if (nb_write_all(c, chunk_hdr, chn) < 0)
			return -1;
		if (nb_write_all(c, page, plen) < 0)
			return -1;
		return nb_write_all(c, "\r\n", 2);
	}

	/* PCM/G.711 — decode to PCM16 LE and write raw (WAV data section) */
	if (codec == RHD_CODEC_PCMU) {
		int16_t pcm[4096];
		uint32_t samples = len > 4096 ? 4096 : len;
		for (uint32_t i = 0; i < samples; i++)
			pcm[i] = ulaw_to_pcm16(data[i]);
		return nb_write_all(c, pcm, samples * 2);
	}

	if (codec == RHD_CODEC_PCMA) {
		int16_t pcm[4096];
		uint32_t samples = len > 4096 ? 4096 : len;
		for (uint32_t i = 0; i < samples; i++)
			pcm[i] = alaw_to_pcm16(data[i]);
		return nb_write_all(c, pcm, samples * 2);
	}

	/* L16 — network byte order to LE (byte-level access for alignment safety) */
	if (codec == RHD_CODEC_L16) {
		int16_t pcm[4096];
		uint32_t samples = len / 2;
		if (samples > 4096)
			samples = 4096;
		for (uint32_t i = 0; i < samples; i++) {
			uint16_t be;
			memcpy(&be, data + i * 2, 2);
			pcm[i] = (int16_t)ntohs(be);
		}
		return nb_write_all(c, pcm, samples * 2);
	}

	/* Unknown codec — write raw */
	return nb_write_all(c, data, len);
}

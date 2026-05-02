/*
 * rac_record.c -- Raptor Audio Client: recording
 *
 * Reads encoded audio from the ring and writes a playable file:
 *   PCMU/PCMA → decoded to PCM16 LE
 *   L16       → byte-swapped to PCM16 LE
 *   AAC       → ADTS-framed (.aac)
 *   Opus      → OGG container (.opus)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <errno.h>

#include "rac.h"

/* ── Codec name lookup ── */

static const char *codec_name(int codec)
{
	switch (codec) {
	case RAC_CODEC_PCMU:
		return "pcmu";
	case RAC_CODEC_PCMA:
		return "pcma";
	case RAC_CODEC_L16:
		return "l16";
	case RAC_CODEC_AAC:
		return "aac";
	case RAC_CODEC_OPUS:
		return "opus";
	default:
		return "unknown";
	}
}

/* ── G.711 mu-law decode ── */

static int16_t ulaw_to_pcm16(uint8_t ulaw)
{
	ulaw = ~ulaw;
	int sign = (ulaw & 0x80);
	int exponent = (ulaw >> 4) & 0x07;
	int mantissa = ulaw & 0x0f;
	int magnitude = ((mantissa << 3) + 0x84) << exponent;
	magnitude -= 0x84;
	return (int16_t)(sign ? -magnitude : magnitude);
}

/* ── G.711 A-law decode ── */

static int16_t alaw_to_pcm16(uint8_t alaw)
{
	alaw ^= 0xD5;
	int sign = (alaw & 0x80);
	int exponent = (alaw >> 4) & 0x07;
	int mantissa = alaw & 0x0f;
	int magnitude;
	if (exponent > 0)
		magnitude = ((mantissa << 4) + 0x108) << (exponent - 1);
	else
		magnitude = (mantissa << 4) + 8;
	return (int16_t)(sign ? magnitude : -magnitude);
}

/* ── ADTS framing for AAC ── */

static int adts_sr_index(int rate)
{
	static const int rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
				    22050, 16000, 12000, 11025, 8000,  7350};
	for (int i = 0; i < 13; i++)
		if (rates[i] == rate)
			return i;
	return 4; /* 44100 fallback */
}

static void write_adts_frame(FILE *out, const uint8_t *data, uint32_t len, int sr_idx)
{
	uint32_t frame_len = len + 7;
	uint8_t hdr[7];
	hdr[0] = 0xFF;
	hdr[1] = 0xF1; /* MPEG-4, Layer 0, no CRC */
	hdr[2] = (1 << 6) | (sr_idx << 2);
	hdr[3] = 0x40 | ((frame_len >> 11) & 0x03);
	hdr[4] = (frame_len >> 3) & 0xFF;
	hdr[5] = ((frame_len & 0x07) << 5) | 0x1F;
	hdr[6] = 0xFC;
	fwrite(hdr, 1, 7, out);
	fwrite(data, 1, len, out);
}

/* ── OGG container for Opus ── */

static uint32_t ogg_crc_table[256];

static void ogg_crc_init(void)
{
	for (int i = 0; i < 256; i++) {
		uint32_t r = (uint32_t)i << 24;
		for (int j = 0; j < 8; j++)
			r = (r << 1) ^ ((r & 0x80000000) ? 0x04C11DB7 : 0);
		ogg_crc_table[i] = r;
	}
}

static uint32_t ogg_crc_update(uint32_t crc, const uint8_t *data, size_t len)
{
	for (size_t i = 0; i < len; i++)
		crc = (crc << 8) ^ ogg_crc_table[((crc >> 24) & 0xFF) ^ data[i]];
	return crc;
}

static void put_le16(uint8_t *p, uint16_t v)
{
	p[0] = v & 0xFF;
	p[1] = (v >> 8) & 0xFF;
}

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = v & 0xFF;
	p[1] = (v >> 8) & 0xFF;
	p[2] = (v >> 16) & 0xFF;
	p[3] = (v >> 24) & 0xFF;
}

static void put_le64(uint8_t *p, int64_t v)
{
	for (int i = 0; i < 8; i++)
		p[i] = (v >> (i * 8)) & 0xFF;
}

static void write_ogg_page(FILE *out, uint32_t serial, uint32_t page_seq, int64_t granule,
			   uint8_t flags, const uint8_t *data, int data_len)
{
	int num_segments = data_len / 255 + 1;
	uint8_t segments[255];
	int remaining = data_len;
	for (int i = 0; i < num_segments; i++) {
		segments[i] = (remaining >= 255) ? 255 : (uint8_t)remaining;
		remaining -= segments[i];
	}

	uint8_t hdr[27];
	memcpy(hdr, "OggS", 4);
	hdr[4] = 0;
	hdr[5] = flags;
	put_le64(hdr + 6, granule);
	put_le32(hdr + 14, serial);
	put_le32(hdr + 18, page_seq);
	memset(hdr + 22, 0, 4);
	hdr[26] = (uint8_t)num_segments;

	uint32_t crc = 0;
	crc = ogg_crc_update(crc, hdr, 27);
	crc = ogg_crc_update(crc, segments, num_segments);
	crc = ogg_crc_update(crc, data, data_len);
	put_le32(hdr + 22, crc);

	fwrite(hdr, 1, 27, out);
	fwrite(segments, 1, num_segments, out);
	fwrite(data, 1, data_len, out);
}

static uint32_t write_opus_headers(FILE *out, uint32_t serial, int sample_rate)
{
	/* OpusHead (RFC 7845 §5.1) */
	uint8_t head[19];
	memcpy(head, "OpusHead", 8);
	head[8] = 1;
	head[9] = 1;
	put_le16(head + 10, 312);
	put_le32(head + 12, (uint32_t)sample_rate);
	put_le16(head + 16, 0);
	head[18] = 0;
	write_ogg_page(out, serial, 0, 0, 0x02, head, 19);

	/* OpusTags (RFC 7845 §5.2) */
	const char *vendor = "raptor";
	uint32_t vlen = (uint32_t)strlen(vendor);
	uint8_t tags[32];
	memcpy(tags, "OpusTags", 8);
	put_le32(tags + 8, vlen);
	memcpy(tags + 12, vendor, vlen);
	put_le32(tags + 12 + vlen, 0);
	write_ogg_page(out, serial, 1, 0, 0x00, tags, 12 + vlen + 4);

	return 2; /* next page_seq */
}

/* ── Record: read from audio ring, write to file ── */

int cmd_record(const char *dest, int max_seconds)
{
	FILE *out = NULL;
	if (strcmp(dest, "-") == 0) {
		out = stdout;
	} else {
		out = fopen(dest, "wb");
		if (!out) {
			fprintf(stderr, "rac: cannot open %s: %s\n", dest, strerror(errno));
			return 1;
		}
	}

	rss_ring_t *ring = rss_ring_open("audio");
	if (!ring) {
		fprintf(stderr, "rac: cannot open audio ring (is RAD running?)\n");
		if (out != stdout)
			fclose(out);
		return 1;
	}
	rss_ring_check_version(ring, "audio");

	const rss_ring_header_t *hdr = rss_ring_get_header(ring);
	int codec = hdr->codec;
	int sample_rate = hdr->fps_num;

	if (out != stdout)
		fprintf(stderr, "rac: recording %s %d Hz from audio ring\n", codec_name(codec),
			sample_rate);

	uint64_t read_seq = 0;
	uint8_t *frame_buf = malloc(rss_ring_max_frame_size(ring));
	if (!frame_buf) {
		fprintf(stderr, "rac: malloc failed\n");
		rss_ring_close(ring);
		if (out != stdout)
			fclose(out);
		return 1;
	}

	/* AAC: precompute ADTS sample rate index */
	int adts_sri = 0;
	if (codec == RAC_CODEC_AAC)
		adts_sri = adts_sr_index(sample_rate);

	/* Opus: write OGG headers, init CRC table and granule tracking.
	 * We buffer one frame so the final page gets the EOS flag. */
	uint32_t ogg_serial = 0;
	uint32_t ogg_page_seq = 0;
	int64_t ogg_granule = 0;
	int opus_frame_samples = 960; /* 20ms at 48kHz (Opus internal rate) */
	uint8_t *ogg_pending = NULL;
	uint32_t ogg_pending_len = 0;
	int64_t ogg_pending_granule = 0;
	if (codec == RAC_CODEC_OPUS) {
		ogg_crc_init();
		ogg_serial = (uint32_t)(rss_timestamp_us() & 0xFFFFFFFF);
		ogg_page_seq = write_opus_headers(out, ogg_serial, sample_rate);
		ogg_pending = malloc(rss_ring_max_frame_size(ring));
		if (!ogg_pending) {
			fprintf(stderr, "rac: malloc failed\n");
			free(frame_buf);
			rss_ring_close(ring);
			if (out != stdout)
				fclose(out);
			return 1;
		}
	}

	int64_t start_time = rss_timestamp_us();
	int64_t max_us = max_seconds > 0 ? (int64_t)max_seconds * 1000000 : 0;
	uint64_t total_bytes = 0;

	while (g_running) {
		if (max_us > 0 && (rss_timestamp_us() - start_time) >= max_us)
			break;

		int ret = rss_ring_wait(ring, 200);
		if (ret != 0)
			continue;

		uint32_t length = 0;
		rss_ring_slot_t meta;
		ret = rss_ring_read(ring, &read_seq, frame_buf, rss_ring_max_frame_size(ring),
				    &length, &meta);
		if (ret != 0)
			continue;

		switch (codec) {
		case RAC_CODEC_PCMU:
			for (uint32_t i = 0; i < length; i++) {
				int16_t sample = ulaw_to_pcm16(frame_buf[i]);
				fwrite(&sample, sizeof(sample), 1, out);
			}
			total_bytes += length * 2;
			break;

		case RAC_CODEC_PCMA:
			for (uint32_t i = 0; i < length; i++) {
				int16_t sample = alaw_to_pcm16(frame_buf[i]);
				fwrite(&sample, sizeof(sample), 1, out);
			}
			total_bytes += length * 2;
			break;

		case RAC_CODEC_L16: {
			uint16_t *samples = (uint16_t *)frame_buf;
			uint32_t n = length / 2;
			for (uint32_t i = 0; i < n; i++) {
				int16_t s = (int16_t)ntohs(samples[i]);
				fwrite(&s, sizeof(s), 1, out);
			}
			total_bytes += length;
			break;
		}

		case RAC_CODEC_AAC:
			write_adts_frame(out, frame_buf, length, adts_sri);
			total_bytes += length + 7;
			break;

		case RAC_CODEC_OPUS:
			if (ogg_pending_len > 0) {
				write_ogg_page(out, ogg_serial, ogg_page_seq++, ogg_pending_granule,
					       0x00, ogg_pending, ogg_pending_len);
				total_bytes += ogg_pending_len;
			}
			ogg_granule += opus_frame_samples;
			ogg_pending_granule = ogg_granule;
			memcpy(ogg_pending, frame_buf, length);
			ogg_pending_len = length;
			break;

		default:
			fwrite(frame_buf, 1, length, out);
			total_bytes += length;
			break;
		}
	}

	/* Flush final Opus page with EOS flag */
	if (ogg_pending_len > 0) {
		write_ogg_page(out, ogg_serial, ogg_page_seq++, ogg_pending_granule, 0x04,
			       ogg_pending, ogg_pending_len);
		total_bytes += ogg_pending_len;
	}

	free(ogg_pending);
	free(frame_buf);
	rss_ring_close(ring);

	if (out != stdout) {
		fclose(out);
		double duration = (double)(rss_timestamp_us() - start_time) / 1000000.0;
		fprintf(stderr, "rac: recorded %.1fs, %llu bytes %s\n", duration,
			(unsigned long long)total_bytes, codec_name(codec));
	}

	return 0;
}

/*
 * rac_record.c -- Raptor Audio Client: recording
 *
 * Read from audio ring, decode G.711 if needed, write PCM16 LE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <errno.h>

#include "rac.h"

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

/* ── Record: read from audio ring, write PCM16 LE ── */

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

	const rss_ring_header_t *hdr = rss_ring_get_header(ring);
	int codec = hdr->codec;
	int sample_rate = hdr->fps_num; /* audio ring stores sample_rate in fps_num */

	if (out != stdout)
		fprintf(stderr, "rac: recording from audio ring, codec=%d, rate=%d Hz\n", codec,
			sample_rate);

	uint64_t read_seq = 0;
	uint8_t *frame_buf = malloc(hdr->data_size);
	if (!frame_buf) {
		fprintf(stderr, "rac: malloc failed\n");
		rss_ring_close(ring);
		if (out != stdout)
			fclose(out);
		return 1;
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
		ret = rss_ring_read(ring, &read_seq, frame_buf, hdr->data_size, &length, &meta);
		if (ret != 0)
			continue;

		/* Decode to PCM16 LE based on codec */
		if (codec == 0) {
			/* PCMU — decode each byte to int16_t */
			for (uint32_t i = 0; i < length; i++) {
				int16_t sample = ulaw_to_pcm16(frame_buf[i]);
				fwrite(&sample, sizeof(sample), 1, out);
			}
			total_bytes += length * 2;
		} else if (codec == 8) {
			/* PCMA — decode each byte to int16_t */
			for (uint32_t i = 0; i < length; i++) {
				int16_t sample = alaw_to_pcm16(frame_buf[i]);
				fwrite(&sample, sizeof(sample), 1, out);
			}
			total_bytes += length * 2;
		} else if (codec == 11) {
			/* L16 — network byte order PCM16, convert to LE */
			uint16_t *samples = (uint16_t *)frame_buf;
			uint32_t n = length / 2;
			for (uint32_t i = 0; i < n; i++) {
				int16_t s = (int16_t)ntohs(samples[i]);
				fwrite(&s, sizeof(s), 1, out);
			}
			total_bytes += length;
		} else {
			/* Unknown codec — write raw */
			fwrite(frame_buf, 1, length, out);
			total_bytes += length;
		}
	}

	free(frame_buf);
	rss_ring_close(ring);

	if (out != stdout) {
		fclose(out);
		double duration = (double)(rss_timestamp_us() - start_time) / 1000000.0;
		fprintf(stderr, "rac: recorded %.1fs, %llu bytes PCM16\n", duration,
			(unsigned long long)total_bytes);
	}

	return 0;
}

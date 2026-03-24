/*
 * rmr_nal.c -- Annex B to AVCC/HVCC NAL conversion
 *
 * Annex B uses start codes (00 00 01 or 00 00 00 01) to delimit NALs.
 * AVCC/HVCC uses 4-byte big-endian length prefixes.
 *
 * The IMP SDK encoder may use either 3-byte or 4-byte start codes.
 * We handle both.
 */

#include "rmr_nal.h"

#include <string.h>

/*
 * Find the next Annex B start code (00 00 01 or 00 00 00 01).
 * Sets *sc_len to 3 or 4. Returns pointer to start of the start code,
 * or NULL if not found.
 */
static const uint8_t *find_start_code(const uint8_t *p, const uint8_t *end, int *sc_len)
{
	while (p + 2 < end) {
		if (p[0] == 0 && p[1] == 0) {
			if (p[2] == 1) {
				*sc_len = 3;
				return p;
			}
			if (p + 3 < end && p[2] == 0 && p[3] == 1) {
				*sc_len = 4;
				return p;
			}
		}
		p++;
	}
	return NULL;
}

static bool h264_is_param(uint8_t first_byte)
{
	uint8_t nal_type = first_byte & 0x1F;
	return (nal_type == 7 || nal_type == 8); /* SPS=7, PPS=8 */
}

static bool h265_is_param(uint8_t first_byte)
{
	uint8_t nal_type = (first_byte >> 1) & 0x3F;
	return (nal_type == 32 || nal_type == 33 || nal_type == 34);
}

int rmr_annexb_to_avcc(const uint8_t *src, uint32_t src_len, uint8_t *dst, uint32_t dst_cap,
		       int codec)
{
	uint32_t out = 0;
	const uint8_t *end = src + src_len;
	int sc_len = 0;
	const uint8_t *sc = find_start_code(src, end, &sc_len);

	while (sc) {
		const uint8_t *nal_start = sc + sc_len;
		if (nal_start >= end)
			break;

		/* Find next start code or end of buffer */
		int next_sc_len;
		const uint8_t *next_sc = find_start_code(nal_start, end, &next_sc_len);

		/* NAL extends to next start code or end */
		const uint8_t *nal_end = next_sc ? next_sc : end;

		/* Trim trailing zero bytes (padding before next start code) */
		while (nal_end > nal_start && nal_end[-1] == 0)
			nal_end--;

		uint32_t nal_len = (uint32_t)(nal_end - nal_start);
		if (nal_len == 0) {
			sc = next_sc;
			sc_len = next_sc_len;
			continue;
		}

		/* Skip parameter set NALs — they go in moov, not mdat */
		bool is_param =
			(codec == 1) ? h265_is_param(nal_start[0]) : h264_is_param(nal_start[0]);

		if (!is_param) {
			if (out + 4 + nal_len > dst_cap)
				return -1;
			/* 4-byte big-endian length prefix */
			dst[out + 0] = (nal_len >> 24) & 0xFF;
			dst[out + 1] = (nal_len >> 16) & 0xFF;
			dst[out + 2] = (nal_len >> 8) & 0xFF;
			dst[out + 3] = nal_len & 0xFF;
			memcpy(dst + out + 4, nal_start, nal_len);
			out += 4 + nal_len;
		}

		sc = next_sc;
		sc_len = next_sc_len;
	}

	return (int)out;
}

void rmr_extract_params(const uint8_t *data, uint32_t len, int codec, rmr_codec_params_t *params)
{
	if (params->ready)
		return;

	const uint8_t *end = data + len;
	int sc_len = 0;
	const uint8_t *sc = find_start_code(data, end, &sc_len);

	while (sc) {
		const uint8_t *nal_start = sc + sc_len;
		if (nal_start >= end)
			break;

		int next_sc_len;
		const uint8_t *next_sc = find_start_code(nal_start, end, &next_sc_len);
		const uint8_t *nal_end = next_sc ? next_sc : end;
		while (nal_end > nal_start && nal_end[-1] == 0)
			nal_end--;

		uint32_t nal_len = (uint32_t)(nal_end - nal_start);
		if (nal_len == 0) {
			sc = next_sc;
			sc_len = next_sc_len;
			continue;
		}

		if (codec == 1) {
			uint8_t nal_type = (nal_start[0] >> 1) & 0x3F;
			if (nal_type == 32 && nal_len <= sizeof(params->vps)) {
				memcpy(params->vps, nal_start, nal_len);
				params->vps_len = nal_len;
			} else if (nal_type == 33 && nal_len <= sizeof(params->sps)) {
				memcpy(params->sps, nal_start, nal_len);
				params->sps_len = nal_len;
			} else if (nal_type == 34 && nal_len <= sizeof(params->pps)) {
				memcpy(params->pps, nal_start, nal_len);
				params->pps_len = nal_len;
			}
		} else {
			uint8_t nal_type = nal_start[0] & 0x1F;
			if (nal_type == 7 && nal_len <= sizeof(params->sps)) {
				memcpy(params->sps, nal_start, nal_len);
				params->sps_len = nal_len;
			} else if (nal_type == 8 && nal_len <= sizeof(params->pps)) {
				memcpy(params->pps, nal_start, nal_len);
				params->pps_len = nal_len;
			}
		}

		sc = next_sc;
		sc_len = next_sc_len;
	}

	if (params->sps_len > 0 && params->pps_len > 0)
		params->ready = true;
}

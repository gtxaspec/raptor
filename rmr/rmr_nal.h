/*
 * rmr_nal.h -- Annex B to AVCC/HVCC NAL conversion
 *
 * Ring data is Annex B format (4-byte start codes: 00 00 00 01).
 * MP4 needs AVCC/HVCC format (4-byte big-endian length prefix).
 *
 * Also extracts SPS/PPS/VPS parameter sets from keyframes
 * for the codec configuration boxes (avcC/hvcC).
 */

#ifndef RMR_NAL_H
#define RMR_NAL_H

#include <stdint.h>
#include <stdbool.h>

/* Extracted codec parameters */
typedef struct {
	uint8_t sps[256];
	uint32_t sps_len;
	uint8_t pps[128];
	uint32_t pps_len;
	uint8_t vps[256]; /* H.265 only */
	uint32_t vps_len;
	bool ready; /* true when at least SPS+PPS are captured */
} rmr_codec_params_t;

/*
 * Convert Annex B NAL stream to AVCC/HVCC length-prefixed format.
 *
 * Input:  [00 00 00 01][NAL1][00 00 00 01][NAL2]...
 * Output: [4-byte-len][NAL1][4-byte-len][NAL2]...
 *
 * Parameter set NALs (SPS/PPS/VPS) are SKIPPED in the output —
 * they belong in the moov box, not in mdat samples.
 *
 * codec: 0=H.264, 1=H.265
 * Returns output length, or -1 on error.
 */
int rmr_annexb_to_avcc(const uint8_t *src, uint32_t src_len, uint8_t *dst, uint32_t dst_cap,
		       int codec);

/*
 * Extract SPS/PPS/VPS from an Annex B keyframe.
 *
 * Scans for parameter set NALs and copies their raw payloads
 * (without start codes) into params. Sets params->ready when
 * at least SPS+PPS are captured.
 *
 * Call on every keyframe until params->ready is true.
 */
void rmr_extract_params(const uint8_t *data, uint32_t len, int codec, rmr_codec_params_t *params);

#endif /* RMR_NAL_H */

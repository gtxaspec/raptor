/*
 * rmr_mux.h -- Fragmented MP4 muxer
 *
 * Standalone fMP4 writer for H.264/H.265 video + PCM/G.711 audio.
 * Output via write callback (no file I/O). Each flush emits one
 * self-contained moof+mdat fragment — crash-safe by design.
 *
 * Usage:
 *   mux = rmr_mux_create(write_fn, ctx);
 *   rmr_mux_set_video(mux, &vp, sps, sps_len, pps, pps_len, NULL, 0);
 *   rmr_mux_set_audio(mux, &ap);
 *   rmr_mux_start(mux);           // writes ftyp + moov
 *   for each frame:
 *     rmr_mux_write_video(mux, &sample);
 *     rmr_mux_write_audio(mux, &sample);
 *     if (keyframe) rmr_mux_flush_fragment(mux);  // writes moof + mdat
 *   rmr_mux_finalize(mux);        // optional mfra
 *   rmr_mux_destroy(mux);
 */

#ifndef RMR_MUX_H
#define RMR_MUX_H

#include <stdint.h>
#include <stdbool.h>

/* Video codec */
#define RMR_CODEC_H264 0
#define RMR_CODEC_H265 1

/* Audio codec (values match RAD ring header codec IDs) */
#define RMR_AUDIO_PCMU 0   /* G.711 mu-law */
#define RMR_AUDIO_PCMA 8   /* G.711 A-law  */
#define RMR_AUDIO_L16  11  /* Linear PCM 16-bit */
#define RMR_AUDIO_AAC  97  /* AAC-LC (raw AUs, no ADTS) */
#define RMR_AUDIO_OPUS 111 /* Opus */

typedef struct rmr_mux rmr_mux_t;

typedef struct {
	uint8_t codec; /* RMR_CODEC_H264 or RMR_CODEC_H265 */
	uint16_t width;
	uint16_t height;
	uint32_t timescale; /* typically 90000 */
} rmr_video_params_t;

typedef struct {
	uint8_t codec;		 /* RMR_AUDIO_PCMU/PCMA/L16 */
	uint32_t sample_rate;	 /* 8000 or 16000 */
	uint8_t channels;	 /* 1 */
	uint8_t bits_per_sample; /* 8 for G.711, 16 for L16 */
} rmr_audio_params_t;

typedef struct {
	const uint8_t *data; /* AVCC/HVCC length-prefixed NALs */
	uint32_t size;
	int64_t dts; /* decode timestamp in timescale units */
	int64_t pts; /* presentation timestamp (= dts for no B-frames) */
	bool is_key;
} rmr_video_sample_t;

typedef struct {
	const uint8_t *data;
	uint32_t size;
	int64_t dts; /* in audio timescale units (sample_rate) */
} rmr_audio_sample_t;

/* Write callback: emit bytes to output sink.
 * Returns 0 on success, -1 on error (muxer stops). */
typedef int (*rmr_write_fn)(const void *buf, uint32_t len, void *ctx);

/* Create/destroy */
rmr_mux_t *rmr_mux_create(rmr_write_fn write_fn, void *write_ctx);
void rmr_mux_destroy(rmr_mux_t *mux);

/* Set codec parameters (must call before rmr_mux_start).
 * SPS/PPS/VPS are raw NAL payloads without start codes.
 * Pass vps=NULL, vps_len=0 for H.264. */
int rmr_mux_set_video(rmr_mux_t *mux, const rmr_video_params_t *params, const uint8_t *sps,
		      uint32_t sps_len, const uint8_t *pps, uint32_t pps_len, const uint8_t *vps,
		      uint32_t vps_len);
int rmr_mux_set_audio(rmr_mux_t *mux, const rmr_audio_params_t *params);

/* Write file header (ftyp + moov). */
int rmr_mux_start(rmr_mux_t *mux);

/* Accumulate samples into current fragment. */
int rmr_mux_write_video(rmr_mux_t *mux, const rmr_video_sample_t *sample);
int rmr_mux_write_audio(rmr_mux_t *mux, const rmr_audio_sample_t *sample);

/* Flush current fragment (moof + mdat) to output. */
int rmr_mux_flush_fragment(rmr_mux_t *mux);

/* Write mfra box for random access (optional, call before destroy). */
int rmr_mux_finalize(rmr_mux_t *mux);

#endif /* RMR_MUX_H */

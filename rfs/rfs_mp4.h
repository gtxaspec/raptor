/*
 * rfs_mp4.h -- MP4/MOV container demuxer for RFS
 *
 * Uses libmov to extract video and audio samples from MP4 files.
 * Produces a frame index with PTS from the container's sample tables,
 * and converts AVCC/HVCC video to Annex B for ring publishing.
 */

#ifndef RFS_MP4_H
#define RFS_MP4_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
	uint64_t file_offset;
	uint32_t length;
	int64_t pts_us;
	uint16_t nal_type;
	uint8_t is_key;
	uint8_t _pad;
} rfs_mp4_frame_t;

typedef struct {
	/* Video */
	rfs_mp4_frame_t *video_frames;
	uint32_t video_count;
	int video_codec;
	uint32_t width;
	uint32_t height;
	uint8_t profile;
	uint8_t level;
	int fps;

	/* Codec config (SPS/PPS/VPS in Annex B) */
	uint8_t *codec_config;
	uint32_t codec_config_len;
	int nal_length_size;

	/* Audio */
	rfs_mp4_frame_t *audio_frames;
	uint32_t audio_count;
	int audio_codec_id;
	int audio_object;
	int audio_sample_rate;
	int audio_channels;
	uint8_t *audio_extra;
	uint32_t audio_extra_len;

	/* Decoded PCM (for transcoding unsupported audio codecs) */
	int16_t *pcm_data;
	uint32_t pcm_samples;
	int pcm_rate;
	bool needs_transcode;

	/* File data */
	uint8_t *file_data;
	size_t file_size;
} rfs_mp4_ctx_t;

int rfs_mp4_open(rfs_mp4_ctx_t *ctx, const char *path);
void rfs_mp4_close(rfs_mp4_ctx_t *ctx);

bool rfs_mp4_is_container(const char *path);

/* Convert AVCC/HVCC sample to Annex B in-place into caller's buffer.
 * Optionally prepend codec config (SPS/PPS/VPS) for keyframes.
 * Returns output length, or -1 on error. */
int rfs_mp4_to_annexb(rfs_mp4_ctx_t *ctx, const rfs_mp4_frame_t *frame,
		      uint8_t *out, uint32_t out_cap);

#endif /* RFS_MP4_H */

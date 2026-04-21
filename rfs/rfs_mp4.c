/*
 * rfs_mp4.c -- MP4/MOV container demuxer for RFS
 *
 * Wraps libmov to extract video and audio samples from MP4 files.
 * Video samples are converted from AVCC/HVCC (length-prefixed) to
 * Annex B (start-code-delimited) for ring publishing. Audio samples
 * are stored as-is (raw codec frames).
 *
 * PTS comes directly from the MP4 sample tables, so B-frame
 * reordering is handled by the container — no heuristics needed.
 */

#include "rfs_mp4.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <rss_common.h>
#include <raptor_hal.h>
#include <mov-reader.h>
#include <mov-format.h>
#include <mov-buffer.h>

#ifdef RAPTOR_MP3
#include <mp3dec.h>
#endif
#ifdef RAPTOR_AAC
#include <aacdec.h>
#endif

#define RFS_MP4_MAX_FRAMES	 65536
#define RFS_MP4_MAX_AUDIO_FRAMES (65536 * 4)
#define RFS_MP4_MAX_FILE_SIZE	 (256 * 1024 * 1024)

static const uint8_t annexb_sc[4] = {0, 0, 0, 1};

/* ── mmap-based mov_buffer_t ── */

typedef struct {
	const uint8_t *data;
	size_t size;
	int64_t pos;
} rfs_mmap_io_t;

static int mmap_read(void *param, void *buf, uint64_t bytes)
{
	rfs_mmap_io_t *io = param;
	if (io->pos < 0 || (uint64_t)io->pos + bytes > io->size)
		return -1;
	memcpy(buf, io->data + io->pos, (size_t)bytes);
	io->pos += (int64_t)bytes;
	return 0;
}

static int mmap_write(void *param, const void *buf, uint64_t bytes)
{
	(void)param;
	(void)buf;
	(void)bytes;
	return -1;
}

static int mmap_seek(void *param, int64_t offset)
{
	rfs_mmap_io_t *io = param;
	if (offset >= 0)
		io->pos = offset;
	else
		io->pos = (int64_t)io->size + offset;
	return 0;
}

static int64_t mmap_tell(void *param)
{
	rfs_mmap_io_t *io = param;
	return io->pos;
}

static const struct mov_buffer_t mmap_buffer = {
	.read = mmap_read,
	.write = mmap_write,
	.seek = mmap_seek,
	.tell = mmap_tell,
};

/* ── AVCC/HVCC extra_data → Annex B codec config ── */

static int parse_avcc_config(const uint8_t *extra, uint32_t extra_len, uint8_t *out,
			     uint32_t out_cap, int *nal_length_size, uint8_t *profile,
			     uint8_t *level)
{
	if (extra_len < 7)
		return -1;

	*nal_length_size = (extra[4] & 0x03) + 1;
	*profile = extra[1];
	*level = extra[3];

	uint32_t pos = 0;
	int sps_count = extra[5] & 0x1F;
	uint32_t idx = 6;

	for (int i = 0; i < sps_count && idx + 2 <= extra_len; i++) {
		uint32_t len = ((uint32_t)extra[idx] << 8) | extra[idx + 1];
		idx += 2;
		if (idx + len > extra_len || pos + 4 + len > out_cap)
			return -1;
		memcpy(out + pos, annexb_sc, 4);
		pos += 4;
		memcpy(out + pos, extra + idx, len);
		pos += len;
		idx += len;
	}

	if (idx >= extra_len)
		return (int)pos;
	int pps_count = extra[idx++];
	for (int i = 0; i < pps_count && idx + 2 <= extra_len; i++) {
		uint32_t len = ((uint32_t)extra[idx] << 8) | extra[idx + 1];
		idx += 2;
		if (idx + len > extra_len || pos + 4 + len > out_cap)
			return -1;
		memcpy(out + pos, annexb_sc, 4);
		pos += 4;
		memcpy(out + pos, extra + idx, len);
		pos += len;
		idx += len;
	}

	return (int)pos;
}

static int parse_hvcc_config(const uint8_t *extra, uint32_t extra_len, uint8_t *out,
			     uint32_t out_cap, int *nal_length_size, uint8_t *profile,
			     uint8_t *level)
{
	if (extra_len < 23)
		return -1;

	*nal_length_size = (extra[21] & 0x03) + 1;
	*profile = extra[1] & 0x1F;
	*level = extra[12];

	int num_arrays = extra[22];
	uint32_t idx = 23;
	uint32_t pos = 0;

	for (int a = 0; a < num_arrays && idx + 3 <= extra_len; a++) {
		idx++; /* array_completeness + nal_unit_type */
		int count = ((int)extra[idx] << 8) | extra[idx + 1];
		idx += 2;
		for (int i = 0; i < count && idx + 2 <= extra_len; i++) {
			uint32_t len = ((uint32_t)extra[idx] << 8) | extra[idx + 1];
			idx += 2;
			if (idx + len > extra_len || pos + 4 + len > out_cap)
				return -1;
			memcpy(out + pos, annexb_sc, 4);
			pos += 4;
			memcpy(out + pos, extra + idx, len);
			pos += len;
			idx += len;
		}
	}

	return (int)pos;
}

/* ── Track info callbacks ── */

typedef struct {
	rfs_mp4_ctx_t *ctx;
	rfs_mmap_io_t *io;
	uint32_t video_track;
	uint32_t audio_track;
	bool has_video;
	bool has_audio;
} rfs_demux_t;

static void on_video(void *param, uint32_t track, uint8_t object, int width, int height,
		     const void *extra, size_t bytes)
{
	rfs_demux_t *d = param;
	if (d->has_video)
		return;

	d->has_video = true;
	d->video_track = track;
	d->ctx->width = (uint32_t)width;
	d->ctx->height = (uint32_t)height;

	if (object == MOV_OBJECT_H264) {
		d->ctx->video_codec = RSS_CODEC_H264;
		uint8_t config[1024];
		int len = parse_avcc_config(extra, (uint32_t)bytes, config, sizeof(config),
					    &d->ctx->nal_length_size, &d->ctx->profile,
					    &d->ctx->level);
		if (len > 0) {
			d->ctx->codec_config = malloc((size_t)len);
			if (d->ctx->codec_config) {
				memcpy(d->ctx->codec_config, config, (size_t)len);
				d->ctx->codec_config_len = (uint32_t)len;
			}
		}
	} else if (object == MOV_OBJECT_H265) {
		d->ctx->video_codec = RSS_CODEC_H265;
		uint8_t config[2048];
		int len = parse_hvcc_config(extra, (uint32_t)bytes, config, sizeof(config),
					    &d->ctx->nal_length_size, &d->ctx->profile,
					    &d->ctx->level);
		if (len > 0) {
			d->ctx->codec_config = malloc((size_t)len);
			if (d->ctx->codec_config) {
				memcpy(d->ctx->codec_config, config, (size_t)len);
				d->ctx->codec_config_len = (uint32_t)len;
			}
		}
	}

	RSS_INFO("mp4 video: track=%u %s %dx%d profile=%u level=%u nalu_len=%d", track,
		 object == MOV_OBJECT_H265 ? "H.265" : "H.264", width, height, d->ctx->profile,
		 d->ctx->level, d->ctx->nal_length_size);
}

static void on_audio(void *param, uint32_t track, uint8_t object, int channel_count,
		     int bit_per_sample, int sample_rate, const void *extra, size_t bytes)
{
	rfs_demux_t *d = param;
	if (d->has_audio)
		return;

	(void)bit_per_sample;

	int codec_id = -1;
	bool transcode = false;

	switch (object) {
	case MOV_OBJECT_AAC:
		codec_id = 97;
		break;
	case MOV_OBJECT_OPUS:
		codec_id = 111;
		break;
	case MOV_OBJECT_G711a:
		codec_id = 8;
		break;
	case MOV_OBJECT_G711u:
		codec_id = 0;
		break;
#ifdef RAPTOR_MP3
	case MOV_OBJECT_MP3:
		transcode = true;
		break;
#endif
	default:
		RSS_WARN("mp4 audio: skipping unsupported object 0x%02x (track %u)", object, track);
		return;
	}

	d->has_audio = true;
	d->audio_track = track;
	d->ctx->audio_object = object;
	d->ctx->audio_codec_id = codec_id;
	d->ctx->needs_transcode = transcode;
	d->ctx->audio_sample_rate = sample_rate;
	d->ctx->audio_channels = channel_count;

	if (bytes > 0) {
		d->ctx->audio_extra = malloc(bytes);
		if (d->ctx->audio_extra) {
			memcpy(d->ctx->audio_extra, extra, bytes);
			d->ctx->audio_extra_len = (uint32_t)bytes;
		}
	}

	RSS_INFO("mp4 audio: track=%u object=0x%02x %dch %dHz", track, object, channel_count,
		 sample_rate);
}

static void on_subtitle(void *param, uint32_t track, uint8_t object, const void *extra,
			size_t bytes)
{
	(void)param;
	(void)track;
	(void)object;
	(void)extra;
	(void)bytes;
}

/* ── Sample read callback ── */

static void on_read(void *param, uint32_t track, const void *buffer, size_t bytes, int64_t pts,
		    int64_t dts, int flags)
{
	rfs_demux_t *d = param;
	(void)buffer;
	(void)dts;

	/* libmov reads each sample as a single seek+read, so the sample's
	 * file offset is the I/O cursor minus the bytes just read. */
	if (d->io->pos < (int64_t)bytes)
		return;
	uint64_t offset = (uint64_t)(d->io->pos - (int64_t)bytes);
	if (offset + bytes > d->ctx->file_size)
		return;

	if (d->has_video && track == d->video_track) {
		if (d->ctx->video_count >= RFS_MP4_MAX_FRAMES)
			return;
		bool is_key = (flags & MOV_AV_FLAG_KEYFREAME) != 0;
		rfs_mp4_frame_t *f = &d->ctx->video_frames[d->ctx->video_count];
		f->file_offset = offset;
		f->length = (uint32_t)bytes;
		f->pts_us = pts * 1000;
		f->is_key = is_key ? 1 : 0;
		f->nal_type = (d->ctx->video_codec == RSS_CODEC_H265)
				      ? (is_key ? RSS_NAL_H265_IDR : RSS_NAL_H265_SLICE)
				      : (is_key ? RSS_NAL_H264_IDR : RSS_NAL_H264_SLICE);
		d->ctx->video_count++;
		return;
	}

	if (d->has_audio && track == d->audio_track) {
		if (d->ctx->audio_count >= RFS_MP4_MAX_AUDIO_FRAMES)
			return;
		rfs_mp4_frame_t *f = &d->ctx->audio_frames[d->ctx->audio_count];
		f->file_offset = offset;
		f->length = (uint32_t)bytes;
		f->pts_us = pts * 1000;
		f->is_key = 0;
		f->nal_type = 0;
		d->ctx->audio_count++;
	}
}

/* ── Audio transcode (decode unsupported codecs to PCM) ── */

#ifdef RAPTOR_MP3
static int decode_mp3_to_pcm(rfs_mp4_ctx_t *ctx)
{
	HMP3Decoder mp3 = MP3InitDecoder();
	if (!mp3)
		return -1;

	uint32_t pcm_cap = 0;
	for (uint32_t i = 0; i < ctx->audio_count; i++)
		pcm_cap += 1152;
	ctx->pcm_data = malloc(pcm_cap * sizeof(int16_t));
	if (!ctx->pcm_data) {
		MP3FreeDecoder(mp3);
		return -1;
	}

	int16_t frame_pcm[2304];
	uint32_t total = 0;
	uint8_t *mp3_buf = malloc(16 * 1024);
	if (!mp3_buf) {
		MP3FreeDecoder(mp3);
		return -1;
	}

	for (uint32_t i = 0; i < ctx->audio_count; i++) {
		uint64_t foff = ctx->audio_frames[i].file_offset;
		uint32_t flen = ctx->audio_frames[i].length;
		if (foff + flen > ctx->file_size)
			continue;
		if (flen > 16 * 1024)
			flen = 16 * 1024;
		memcpy(mp3_buf, ctx->file_data + foff, flen);
		unsigned char *ptr = mp3_buf;
		int remaining = (int)flen;

		while (remaining > 0) {
			int offset = MP3FindSyncWord(ptr, remaining);
			if (offset < 0)
				break;
			ptr += offset;
			remaining -= offset;

			unsigned char *read_ptr = ptr;
			int err = MP3Decode(mp3, &read_ptr, &remaining, frame_pcm, 0);
			if (err) {
				if (remaining > 1) {
					ptr++;
					remaining--;
				} else {
					break;
				}
				continue;
			}
			ptr = read_ptr;

			MP3FrameInfo info;
			MP3GetLastFrameInfo(mp3, &info);
			int samples = info.outputSamps / info.nChans;

			if (total + (uint32_t)samples > pcm_cap) {
				pcm_cap = (total + (uint32_t)samples) * 2;
				int16_t *tmp = realloc(ctx->pcm_data, pcm_cap * sizeof(int16_t));
				if (!tmp) {
					MP3FreeDecoder(mp3);
					return -1;
				}
				ctx->pcm_data = tmp;
			}

			if (info.nChans == 2) {
				for (int j = 0; j < samples; j++)
					ctx->pcm_data[total + (uint32_t)j] =
						(int16_t)((frame_pcm[j * 2] +
							   frame_pcm[j * 2 + 1]) /
							  2);
			} else {
				memcpy(ctx->pcm_data + total, frame_pcm,
				       (size_t)samples * sizeof(int16_t));
			}
			total += (uint32_t)samples;
			ctx->pcm_rate = info.samprate;
		}
	}

	free(mp3_buf);
	MP3FreeDecoder(mp3);
	ctx->pcm_samples = total;

	free(ctx->audio_frames);
	ctx->audio_frames = NULL;
	ctx->audio_count = 0;

	RSS_INFO("mp4: decoded MP3 → %u PCM samples @ %d Hz", ctx->pcm_samples, ctx->pcm_rate);
	return 0;
}
#endif

/* ── Public API ── */

bool rfs_mp4_is_container(const char *path)
{
	const char *dot = strrchr(path, '.');
	if (!dot)
		return false;
	return strcasecmp(dot, ".mp4") == 0 || strcasecmp(dot, ".mov") == 0 ||
	       strcasecmp(dot, ".m4v") == 0 || strcasecmp(dot, ".m4a") == 0;
}

int rfs_mp4_open(rfs_mp4_ctx_t *ctx, const char *path)
{
	memset(ctx, 0, sizeof(*ctx));

	/* mmap the file */
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		RSS_ERROR("mp4: open %s: %s", path, strerror(errno));
		return -1;
	}

	struct stat sb;
	if (fstat(fd, &sb) < 0 || sb.st_size == 0 || sb.st_size > RFS_MP4_MAX_FILE_SIZE) {
		RSS_ERROR("mp4: %s: invalid size", path);
		close(fd);
		return -1;
	}

	ctx->file_data = mmap(NULL, (size_t)sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (ctx->file_data == MAP_FAILED) {
		ctx->file_data = NULL;
		RSS_ERROR("mp4: mmap %s: %s", path, strerror(errno));
		return -1;
	}
	ctx->file_size = (size_t)sb.st_size;

	/* Set up I/O */
	rfs_mmap_io_t io = {.data = ctx->file_data, .size = ctx->file_size, .pos = 0};

	mov_reader_t *mov = mov_reader_create(&mmap_buffer, &io);
	if (!mov) {
		RSS_ERROR("mp4: failed to parse %s", path);
		rfs_mp4_close(ctx);
		return -1;
	}

	/* Allocate frame arrays */
	ctx->video_frames = calloc(RFS_MP4_MAX_FRAMES, sizeof(rfs_mp4_frame_t));
	ctx->audio_frames = calloc(RFS_MP4_MAX_AUDIO_FRAMES, sizeof(rfs_mp4_frame_t));
	if (!ctx->video_frames || !ctx->audio_frames) {
		RSS_ERROR("mp4: frame alloc failed");
		mov_reader_destroy(mov);
		rfs_mp4_close(ctx);
		return -1;
	}

	/* Query track info */
	rfs_demux_t demux = {.ctx = ctx, .io = &io};
	struct mov_reader_trackinfo_t info = {
		.onvideo = on_video,
		.onaudio = on_audio,
		.onsubtitle = on_subtitle,
	};
	mov_reader_getinfo(mov, &info, &demux);

	if (!demux.has_video && !demux.has_audio) {
		RSS_ERROR("mp4: no supported tracks in %s", path);
		mov_reader_destroy(mov);
		rfs_mp4_close(ctx);
		return -1;
	}

	/* Read all samples — we only capture metadata (offsets) in on_read,
	 * but libmov requires a buffer for the data it reads. Size it to
	 * the largest expected frame (256KB covers 1080p keyframes). */
	enum { SAMPLE_BUF_SIZE = 2 * 1024 * 1024 };
	uint8_t *sample_buf = malloc(SAMPLE_BUF_SIZE);
	if (!sample_buf) {
		mov_reader_destroy(mov);
		rfs_mp4_close(ctx);
		return -1;
	}

	while (mov_reader_read(mov, sample_buf, SAMPLE_BUF_SIZE, on_read, &demux) > 0) {
		if (ctx->video_count >= RFS_MP4_MAX_FRAMES &&
		    ctx->audio_count >= RFS_MP4_MAX_AUDIO_FRAMES)
			break;
	}

	free(sample_buf);
	mov_reader_destroy(mov);

	/* Transcode unsupported audio codecs to PCM */
	if (ctx->needs_transcode && ctx->audio_count > 0) {
#ifdef RAPTOR_MP3
		if (ctx->audio_object == MOV_OBJECT_MP3) {
			if (decode_mp3_to_pcm(ctx) < 0)
				RSS_WARN("mp4: MP3 decode failed, continuing without audio");
		} else
#endif
		{
			RSS_WARN("mp4: no decoder for audio object 0x%02x", ctx->audio_object);
			ctx->needs_transcode = false;
		}
	}

	/* Detect FPS from video PTS */
	if (ctx->video_count >= 2) {
		int64_t total_dur = ctx->video_frames[ctx->video_count - 1].pts_us -
				    ctx->video_frames[0].pts_us;
		if (total_dur > 0)
			ctx->fps = (int)(((int64_t)(ctx->video_count - 1) * 1000000LL +
					  total_dur / 2) /
					 total_dur);
		if (ctx->fps <= 0)
			ctx->fps = 30;
	} else {
		ctx->fps = 30;
	}

	uint32_t keys = 0;
	for (uint32_t i = 0; i < ctx->video_count; i++)
		if (ctx->video_frames[i].is_key)
			keys++;

	RSS_INFO("mp4: %u video frames (%u key, %dfps), %u audio frames", ctx->video_count, keys,
		 ctx->fps, ctx->audio_count);

	return 0;
}

void rfs_mp4_close(rfs_mp4_ctx_t *ctx)
{
	free(ctx->video_frames);
	free(ctx->audio_frames);
	free(ctx->pcm_data);
	free(ctx->codec_config);
	free(ctx->audio_extra);
	if (ctx->file_data)
		munmap(ctx->file_data, ctx->file_size);
	memset(ctx, 0, sizeof(*ctx));
}

int rfs_mp4_to_annexb(rfs_mp4_ctx_t *ctx, const rfs_mp4_frame_t *frame, uint8_t *out,
		      uint32_t out_cap)
{
	if (frame->file_offset + frame->length > ctx->file_size)
		return -1;

	uint32_t pos = 0;
	const uint8_t *src = ctx->file_data + frame->file_offset;
	uint32_t src_len = frame->length;

	/* Prepend codec config (SPS/PPS/VPS) on keyframes */
	if (frame->is_key && ctx->codec_config_len > 0) {
		if (ctx->codec_config_len > out_cap)
			return -1;
		memcpy(out, ctx->codec_config, ctx->codec_config_len);
		pos = ctx->codec_config_len;
	}

	/* Convert AVCC length-prefixed NALs to Annex B start codes */
	uint32_t spos = 0;
	while (spos + (uint32_t)ctx->nal_length_size <= src_len) {
		uint32_t nal_len = 0;
		for (int i = 0; i < ctx->nal_length_size; i++)
			nal_len = (nal_len << 8) | src[spos++];
		if (spos + nal_len > src_len)
			break;
		if (pos + 4 + nal_len > out_cap)
			return -1;
		out[pos++] = 0;
		out[pos++] = 0;
		out[pos++] = 0;
		out[pos++] = 1;
		memcpy(out + pos, src + spos, nal_len);
		pos += nal_len;
		spos += nal_len;
	}

	return (int)pos;
}

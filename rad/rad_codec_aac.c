/*
 * rad_codec_aac.c -- AAC-LC encoder via faac
 *
 * faac requires a fixed number of input samples per frame (typically
 * 1024), but the HAL delivers smaller chunks (e.g., 320 samples).
 * This codec accumulates PCM in an internal buffer and encodes when
 * full, publishing directly to the ring to avoid returning partial
 * frames to the caller.
 */

#ifdef RAPTOR_AAC

#include "rad.h"
#include <stdlib.h>
#include <string.h>
#include <faac.h>
#include <rss_ipc.h>

typedef struct {
	faacEncHandle handle;
	int16_t *pcm_buf;	  /* accumulation buffer */
	int pcm_fill;		  /* samples accumulated */
	int frame_samples;	  /* samples needed per AAC frame */
	unsigned long max_output; /* max encoded bytes per frame */
} aac_state_t;

static int aac_init(rad_codec_ctx_t *ctx, rss_config_t *cfg, int sample_rate)
{
	aac_state_t *st = calloc(1, sizeof(*st));
	if (!st)
		return -1;

	unsigned long input_samples = 0;
	st->handle = faacEncOpen(sample_rate, 1, &input_samples, &st->max_output);
	if (!st->handle) {
		free(st);
		return -1;
	}

	faacEncConfigurationPtr aac_cfg = faacEncGetCurrentConfiguration(st->handle);
	aac_cfg->aacObjectType = LOW;
	aac_cfg->mpegVersion = MPEG4;
	aac_cfg->inputFormat = FAAC_INPUT_16BIT;
	aac_cfg->outputFormat = RAW_STREAM;
	aac_cfg->bandWidth = sample_rate;
	int bitrate = rss_config_get_int(cfg, "audio", "bitrate", 32000);
	if (bitrate > 256000)
		bitrate = 256000;
	aac_cfg->bitRate = bitrate;
	aac_cfg->allowMidside = 0;
	aac_cfg->useTns = 0;
	if (!faacEncSetConfiguration(st->handle, aac_cfg)) {
		faacEncClose(st->handle);
		free(st);
		return -1;
	}

	st->frame_samples = (int)input_samples;
	st->pcm_buf = malloc(st->frame_samples * sizeof(int16_t));
	if (!st->pcm_buf) {
		faacEncClose(st->handle);
		free(st);
		return -1;
	}
	st->pcm_fill = 0;

	ctx->priv = st;
	ctx->encode_buf_size = (int)st->max_output;

	RSS_DEBUG("aac encoder: %d samples/frame, max %lu bytes output", st->frame_samples,
		  st->max_output);
	return 0;
}

static int aac_encode(rad_codec_ctx_t *ctx, const int16_t *pcm, int samples, uint8_t *out,
		      int out_size, int64_t timestamp)
{
	aac_state_t *st = ctx->priv;
	const int16_t *src = pcm;
	int remaining = samples;

	while (remaining > 0) {
		int copy = remaining;
		if (st->pcm_fill + copy > st->frame_samples)
			copy = st->frame_samples - st->pcm_fill;
		memcpy(st->pcm_buf + st->pcm_fill, src, copy * sizeof(int16_t));
		st->pcm_fill += copy;
		src += copy;
		remaining -= copy;

		if (st->pcm_fill >= st->frame_samples) {
			int len = faacEncEncode(st->handle, (int32_t *)st->pcm_buf,
						st->frame_samples, out, out_size);
			st->pcm_fill = 0;
			if (len > 0 && ctx->ring)
				rss_ring_publish(ctx->ring, out, len, timestamp, ctx->codec_id, 0);
		}
	}

	/* AAC publishes internally — return 0 so caller doesn't double-publish */
	return 0;
}

static void aac_deinit(rad_codec_ctx_t *ctx)
{
	aac_state_t *st = ctx->priv;
	if (!st)
		return;
	if (st->handle)
		faacEncClose(st->handle);
	free(st->pcm_buf);
	free(st);
	ctx->priv = NULL;
}

const rad_codec_ops_t rad_codec_aac = {
	.name = "aac",
	.codec_id = RAD_CODEC_AAC,
	.init = aac_init,
	.encode = aac_encode,
	.deinit = aac_deinit,
};

#endif /* RAPTOR_AAC */

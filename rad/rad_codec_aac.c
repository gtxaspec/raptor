/*
 * rad_codec_aac.c -- AAC encoder via faac (AAC-LC or HE-AAC, auto-selected)
 *
 * faac batches input internally and emits one AAC frame per
 * frame_samples (1024 for AAC-LC, 2048 for HE-AAC, resolved via
 * faac_encoder_get_info()), so HAL chunks (e.g., 320 samples) are fed
 * straight to the encoder. Only a fill counter is kept, to know which
 * chunk started each encoder frame for timestamping. Frames are
 * published directly to the ring to avoid returning partial frames to
 * the caller.
 */

#ifdef RAPTOR_AAC

#include "rad.h"
#include <stdlib.h>
#include <faac.h>
#include <rss_ipc.h>

typedef struct {
	faac_encoder *handle;
	int frame_fill;	     /* samples fed into the current encoder frame */
	int frame_samples;   /* samples per AAC frame */
	uint32_t max_output; /* max encoded bytes per frame */
	int64_t frame_ts;    /* timestamp of first chunk in current frame */
} aac_state_t;

static int aac_init(rad_codec_ctx_t *ctx, rss_config_t *cfg, int sample_rate)
{
	aac_state_t *st = calloc(1, sizeof(*st));
	if (!st)
		return -1;

	int bitrate = rss_config_get_int(cfg, "audio", "bitrate", 128000);
	if (bitrate > 256000)
		bitrate = 256000;

	faac_params params;
	faac_status status = faac_params_init(&params);
	if (status != FAAC_OK) {
		free(st);
		return -1;
	}
	params.sample_rate = (uint32_t)sample_rate;
	params.num_channels = 1;
	params.mpeg_version = FAAC_MPEG4;
	params.object_type = FAAC_OBJ_AUTO;
	params.joint_mode = FAAC_JOINT_NONE;
	params.use_tns = false;
	params.bit_rate = (uint32_t)bitrate; /* per channel; mono */
	params.bandwidth = 0;		     /* derive cutoff from bit_rate */
	params.output_format = FAAC_STREAM_RAW;
	params.input_format = FAAC_INPUT_16BIT;

	status = faac_encoder_open(&params, &st->handle);
	if (status != FAAC_OK) {
		RSS_ERROR("aac encoder open failed: %s", faac_strerror(status));
		free(st);
		return -1;
	}

	faac_encoder_info info = {.struct_size = sizeof(info)};
	status = faac_encoder_get_info(st->handle, &info);
	if (status != FAAC_OK) {
		RSS_ERROR("aac encoder info failed: %s", faac_strerror(status));
		faac_encoder_close(&st->handle);
		free(st);
		return -1;
	}

	st->frame_samples = (int)info.frame_samples; /* mono: total == per channel */
	st->max_output = info.max_output_bytes;

	ctx->priv = st;
	ctx->encode_buf_size = (int)st->max_output;

	RSS_DEBUG("aac encoder: %d samples/frame, max %u bytes output", st->frame_samples,
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
		if (st->frame_fill == 0)
			st->frame_ts = timestamp;
		int chunk = remaining;
		if (chunk > st->frame_samples)
			chunk = st->frame_samples;

		uint32_t len = 0;
		faac_status status = faac_encoder_encode(st->handle, src, (uint32_t)chunk, out,
							 (uint32_t)out_size, &len);
		if (status != FAAC_OK) {
			RSS_WARN("aac encode failed: %s", faac_strerror(status));
			return 0;
		}

		int64_t ts = st->frame_ts;
		st->frame_fill += chunk;
		if (st->frame_fill >= st->frame_samples) {
			st->frame_fill -= st->frame_samples;
			/* tail of this chunk started the next encoder frame */
			if (st->frame_fill > 0)
				st->frame_ts = timestamp;
		}

		if (len > 0 && ctx->ring)
			rss_ring_publish(ctx->ring, out, len, ts, ctx->codec_id, 0);

		src += chunk;
		remaining -= chunk;
	}

	/* AAC publishes internally — return 0 so caller doesn't double-publish */
	return 0;
}

static void aac_deinit(rad_codec_ctx_t *ctx)
{
	aac_state_t *st = ctx->priv;
	if (!st)
		return;
	faac_encoder_close(&st->handle);
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

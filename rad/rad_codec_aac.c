/*
 * rad_codec_aac.c -- AAC encoder via faac (AAC-LC or HE-AAC v1)
 *
 * faac batches input internally and emits one AAC frame per
 * frame_samples (1024 LC, 2048 HE-AAC; resolved via
 * faac_encoder_get_info()), so HAL chunks (e.g., 320 samples) are fed
 * straight to the encoder. Only a fill counter is kept, to know which
 * chunk started each encoder frame for timestamping. Frames are
 * published directly to the ring to avoid returning partial frames to
 * the caller.
 *
 * [audio] aac_profile selects the object type: lc (default), he, or
 * auto (faac picks by rate/bitrate). The resolved frame size and
 * object type are exported through ctx so RAD can publish them in the
 * ring stream info for consumers (RSD cadence + SDP config, RMR esds,
 * ADTS emitters).
 */

#ifdef RAPTOR_AAC

#include "rad.h"
#include <stdlib.h>
#include <string.h>
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

	/* HE-AAC needs Fs >= 32kHz (the Fs/2 SBR core collapses below);
	 * clamp an explicit request rather than failing encoder open. */
	const char *profile = rss_config_get_str(cfg, "audio", "aac_profile", "lc");
	enum faac_object_type object = FAAC_OBJ_LOW;
	if (strcmp(profile, "he") == 0) {
		if (sample_rate >= 32000) {
			object = FAAC_OBJ_HE_AAC_V1;
		} else {
			RSS_WARN("aac_profile=he needs sample_rate >= 32000 (have %d), using lc",
				 sample_rate);
		}
	} else if (strcmp(profile, "auto") == 0) {
		object = FAAC_OBJ_AUTO;
	} else if (strcmp(profile, "lc") != 0) {
		RSS_WARN("unknown aac_profile \"%s\" (lc|he|auto), using lc", profile);
	}

	faac_params params;
	faac_status status = faac_params_init(&params);
	if (status != FAAC_OK) {
		free(st);
		return -1;
	}
	params.sample_rate = (uint32_t)sample_rate;
	params.num_channels = 1;
	params.mpeg_version = FAAC_MPEG4;
	params.object_type = object;
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
	ctx->frame_samples = st->frame_samples;
	ctx->aot = (info.object_type == FAAC_OBJ_HE_AAC_V1) ? 5 : 2;

	const uint8_t *asc = NULL;
	uint32_t asc_len = 0;
	(void)!faac_encoder_asc(st->handle, &asc, &asc_len);
	RSS_INFO("aac encoder: %s, %d samples/frame, asc %02X%02X%02X%02X (%u bytes)",
		 ctx->aot == 5 ? "HE-AAC v1" : "AAC-LC", st->frame_samples,
		 asc_len > 0 ? asc[0] : 0, asc_len > 1 ? asc[1] : 0, asc_len > 2 ? asc[2] : 0,
		 asc_len > 3 ? asc[3] : 0, asc_len);
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

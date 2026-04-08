/*
 * rad_codec_opus.c -- Opus encoder via libopus
 */

#ifdef RAPTOR_OPUS

#include "rad.h"
#include <stdlib.h>
#include <opus/opus.h>

typedef struct {
	OpusEncoder *enc;
} opus_state_t;

static int opus_init_codec(rad_codec_ctx_t *ctx, rss_config_t *cfg, int sample_rate)
{
	opus_state_t *st = calloc(1, sizeof(*st));
	if (!st)
		return -1;

	int err;
	st->enc = opus_encoder_create(sample_rate, 1, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
	if (err != OPUS_OK || !st->enc) {
		free(st);
		return -1;
	}

	int bitrate = rss_config_get_int(cfg, "audio", "bitrate", 32000);
	if (bitrate > 256000)
		bitrate = 256000;
	opus_encoder_ctl(st->enc, OPUS_SET_BITRATE(bitrate));

	int complexity = rss_config_get_int(cfg, "audio", "opus_complexity", 5);
	if (complexity < 0)
		complexity = 0;
	if (complexity > 10)
		complexity = 10;
	opus_encoder_ctl(st->enc, OPUS_SET_COMPLEXITY(complexity));

	ctx->priv = st;
	ctx->encode_buf_size = 4096;

	RSS_DEBUG("opus encoder: bitrate=%d complexity=%d", bitrate, complexity);
	return 0;
}

static int opus_encode_frame(rad_codec_ctx_t *ctx, const int16_t *pcm, int samples, uint8_t *out,
			     int out_size, int64_t timestamp)
{
	(void)timestamp;
	opus_state_t *st = ctx->priv;
	int len = opus_encode(st->enc, pcm, samples, out, out_size);
	if (len < 0) {
		RSS_WARN("opus encode failed: %d", len);
		return 0;
	}
	return len;
}

static void opus_deinit(rad_codec_ctx_t *ctx)
{
	opus_state_t *st = ctx->priv;
	if (!st)
		return;
	if (st->enc)
		opus_encoder_destroy(st->enc);
	free(st);
	ctx->priv = NULL;
}

const rad_codec_ops_t rad_codec_opus = {
	.name = "opus",
	.codec_id = RAD_CODEC_OPUS,
	.init = opus_init_codec,
	.encode = opus_encode_frame,
	.deinit = opus_deinit,
};

#endif /* RAPTOR_OPUS */

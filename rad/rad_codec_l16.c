/*
 * rad_codec_l16.c -- Uncompressed 16-bit PCM (network byte order)
 */

#include "rad.h"
#include <stddef.h>
#include <arpa/inet.h>

static int l16_init(rad_codec_ctx_t *ctx, rss_config_t *cfg, int sample_rate)
{
	(void)cfg;
	(void)sample_rate;
	ctx->encode_buf_size = 2048; /* 2 bytes per sample */
	return 0;
}

static int l16_encode(rad_codec_ctx_t *ctx, const int16_t *pcm, int samples, uint8_t *out,
		      int out_size, int64_t timestamp)
{
	(void)ctx;
	(void)timestamp;
	if (samples * 2 > out_size)
		samples = out_size / 2;
	uint16_t *dst = (uint16_t *)out;
	for (int i = 0; i < samples; i++)
		dst[i] = htons((uint16_t)pcm[i]);
	return samples * 2;
}

const rad_codec_ops_t rad_codec_l16 = {
	.name = "l16",
	.codec_id = RAD_CODEC_L16,
	.init = l16_init,
	.encode = l16_encode,
	.deinit = NULL,
};

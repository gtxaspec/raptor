/*
 * rad_codec_g711.c -- G.711 mu-law (PCMU) and A-law (PCMA) encoders
 */

#include "rad.h"
#include <stddef.h>
#include <stdint.h>

/* ── mu-law ── */

static uint8_t pcm16_to_ulaw(int16_t pcm)
{
	int sign, exponent, mantissa;
	const int BIAS = 0x84;

	if (pcm == -32768)
		pcm = -32767;
	sign = (pcm >> 8) & 0x80;
	if (sign)
		pcm = -pcm;
	if (pcm > 32635)
		pcm = 32635;
	pcm += BIAS;

	exponent = 7;
	for (int mask = 0x4000; !(pcm & mask) && exponent > 0; exponent--, mask >>= 1)
		;

	mantissa = (pcm >> (exponent + 3)) & 0x0f;
	return ~(sign | (exponent << 4) | mantissa);
}

static int pcmu_init(rad_codec_ctx_t *ctx, rss_config_t *cfg, int sample_rate)
{
	(void)cfg;
	(void)sample_rate;
	ctx->encode_buf_size = 1024; /* 1 byte per sample */
	return 0;
}

static int pcmu_encode(rad_codec_ctx_t *ctx, const int16_t *pcm, int samples, uint8_t *out,
		       int out_size, int64_t timestamp)
{
	(void)ctx;
	(void)timestamp;
	if (samples > out_size)
		samples = out_size;
	for (int i = 0; i < samples; i++)
		out[i] = pcm16_to_ulaw(pcm[i]);
	return samples;
}

const rad_codec_ops_t rad_codec_pcmu = {
	.name = "pcmu",
	.codec_id = RAD_CODEC_PCMU,
	.init = pcmu_init,
	.encode = pcmu_encode,
	.deinit = NULL,
};

/* ── A-law ── */

static uint8_t pcm16_to_alaw(int16_t pcm)
{
	int sign, exponent, mantissa;

	if (pcm == -32768)
		pcm = -32767;
	sign = ((~pcm) >> 8) & 0x80;
	if (!sign)
		pcm = -pcm;
	if (pcm > 32635)
		pcm = 32635;

	if (pcm >= 256) {
		exponent = 7;
		for (int mask = 0x4000; !(pcm & mask) && exponent > 1; exponent--, mask >>= 1)
			;
		mantissa = (pcm >> (exponent + 3)) & 0x0f;
	} else {
		exponent = 0;
		mantissa = (pcm >> 4) & 0x0f;
	}

	return (sign | (exponent << 4) | mantissa) ^ 0xD5;
}

static int pcma_init(rad_codec_ctx_t *ctx, rss_config_t *cfg, int sample_rate)
{
	(void)cfg;
	(void)sample_rate;
	ctx->encode_buf_size = 1024;
	return 0;
}

static int pcma_encode(rad_codec_ctx_t *ctx, const int16_t *pcm, int samples, uint8_t *out,
		       int out_size, int64_t timestamp)
{
	(void)ctx;
	(void)timestamp;
	if (samples > out_size)
		samples = out_size;
	for (int i = 0; i < samples; i++)
		out[i] = pcm16_to_alaw(pcm[i]);
	return samples;
}

const rad_codec_ops_t rad_codec_pcma = {
	.name = "pcma",
	.codec_id = RAD_CODEC_PCMA,
	.init = pcma_init,
	.encode = pcma_encode,
	.deinit = NULL,
};

/*
 * rsp_audio.c -- Audio transcode for RTMP push
 *
 * Decodes G.711 µ-law/A-law, L16, Opus, or AAC from the ring to
 * PCM, then encodes to AAC-LC via faac. The faac encoder requires
 * 1024 input samples per frame, so PCM is accumulated internally.
 *
 * Requires RAPTOR_AAC_ENC (faac). Optional: RAPTOR_OPUS (libopus
 * decode), RAPTOR_AAC (helix-aac decode for AAC re-encode).
 */

#ifndef RAPTOR_AAC_ENC

#include "rsp_audio.h"
#include <rss_common.h>

/* Stub when faac is not available */
rsp_audio_enc_t *rsp_audio_init(uint32_t input_codec, uint32_t sample_rate)
{
	(void)input_codec;
	(void)sample_rate;
	RSS_WARN("rsp_audio: transcode not available (no faac)");
	return NULL;
}

int rsp_audio_transcode(rsp_audio_enc_t *enc, const uint8_t *data, uint32_t len,
			uint32_t timestamp_ms, rsp_audio_cb cb, void *userdata)
{
	(void)enc;
	(void)data;
	(void)len;
	(void)timestamp_ms;
	(void)cb;
	(void)userdata;
	return -1;
}

void rsp_audio_reset(rsp_audio_enc_t *enc)
{
	(void)enc;
}

void rsp_audio_free(rsp_audio_enc_t *enc)
{
	(void)enc;
}

#else /* RAPTOR_AAC_ENC */

#include "rsp_audio.h"

#include <rss_common.h>

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include <faac.h>

#ifdef RAPTOR_OPUS
#include <opus/opus.h>
#endif

#ifdef RAPTOR_AAC
#include <aacdec.h>
#endif

/* ── G.711 tables ── */

static int16_t ulaw_table[256];
static int16_t alaw_table[256];
static _Atomic bool tables_init;

static void g711_init_tables(void)
{
	if (atomic_load(&tables_init))
		return;
	for (int i = 0; i < 256; i++) {
		/* µ-law decode */
		uint8_t u = ~(uint8_t)i;
		int sign = (u & 0x80) ? -1 : 1;
		int exp = (u >> 4) & 0x07;
		int mantissa = u & 0x0F;
		int mag = ((mantissa << 1) + 33) << (exp + 2);
		ulaw_table[i] = (int16_t)(sign * (mag - (1 << 5)));

		/* A-law decode */
		uint8_t a = (uint8_t)i ^ 0x55;
		sign = (a & 0x80) ? 1 : -1;
		exp = (a >> 4) & 0x07;
		mantissa = a & 0x0F;
		if (exp == 0)
			mag = (mantissa << 1) + 1;
		else
			mag = ((mantissa << 1) + 33) << (exp - 1);
		alaw_table[i] = (int16_t)(sign * mag);
	}
	atomic_store(&tables_init, true);
}

/* ── Transcoder state ── */

#define RSP_AAC_OUTPUT_RATE 48000

struct rsp_audio_enc {
	uint32_t input_codec;
	uint32_t input_rate;
	uint32_t output_rate;

	/* faac encoder (always at output_rate) */
	faacEncHandle faac;
	int frame_samples; /* samples per AAC frame (1024 at output_rate) */
	unsigned long max_output;

	/* PCM accumulation buffer (at output_rate) */
	int16_t *pcm_buf;
	int pcm_fill;

	/* AAC output buffer */
	uint8_t *aac_buf;

	/* Resample state (non-integer ratio fallback) */
	uint32_t resample_frac;

	/* Smooth output timestamp (advances by frame_dur per output frame,
	 * corrected toward ring timestamp to prevent long-term drift) */
	uint32_t next_out_ts;
	bool out_ts_running;

	/* Decode state */
#ifdef RAPTOR_OPUS
	OpusDecoder *opus_dec;
#endif
#ifdef RAPTOR_AAC
	HAACDecoder aac_dec;
#endif
};

rsp_audio_enc_t *rsp_audio_init(uint32_t input_codec, uint32_t sample_rate)
{
	rsp_audio_enc_t *enc = calloc(1, sizeof(*enc));
	if (!enc)
		return NULL;

	enc->input_codec = input_codec;
	enc->input_rate = sample_rate;
	enc->output_rate = RSP_AAC_OUTPUT_RATE;

	g711_init_tables();

	/* Init faac encoder at output rate (48kHz for RTMP compatibility) */
	unsigned long input_samples = 0;
	enc->faac = faacEncOpen(enc->output_rate, 1, &input_samples, &enc->max_output);
	if (!enc->faac) {
		RSS_ERROR("rsp_audio: faac open failed");
		free(enc);
		return NULL;
	}

	faacEncConfigurationPtr cfg = faacEncGetCurrentConfiguration(enc->faac);
	cfg->aacObjectType = LOW;
	cfg->mpegVersion = MPEG4;
	cfg->inputFormat = FAAC_INPUT_16BIT;
	cfg->outputFormat = RAW_STREAM;
	cfg->bandWidth = enc->output_rate / 2;
	cfg->bitRate = 128000;
	cfg->allowMidside = 0;
	cfg->useTns = 0;
	if (!faacEncSetConfiguration(enc->faac, cfg)) {
		RSS_ERROR("rsp_audio: faac config failed");
		faacEncClose(enc->faac);
		free(enc);
		return NULL;
	}

	enc->frame_samples = (int)input_samples;
	enc->pcm_buf = malloc((size_t)enc->frame_samples * sizeof(int16_t));
	enc->aac_buf = malloc(enc->max_output);
	if (!enc->pcm_buf || !enc->aac_buf) {
		faacEncClose(enc->faac);
		free(enc->pcm_buf);
		free(enc->aac_buf);
		free(enc);
		return NULL;
	}

	/* Init decoder for input codec */
#ifdef RAPTOR_OPUS
	if (input_codec == RSP_CODEC_OPUS) {
		int err;
		enc->opus_dec = opus_decoder_create((opus_int32)enc->output_rate, 1, &err);
		if (!enc->opus_dec) {
			RSS_WARN("rsp_audio: opus decoder init failed");
		} else {
			enc->input_rate = enc->output_rate;
			RSS_DEBUG("rsp_audio: opus decode at %uHz (native)", enc->output_rate);
		}
	}
#endif

#ifdef RAPTOR_AAC
	if (input_codec == RSP_CODEC_AAC) {
		enc->aac_dec = AACInitDecoder();
		if (!enc->aac_dec) {
			RSS_WARN("rsp_audio: AAC decoder init failed");
		} else {
			AACFrameInfo fi = {0};
			fi.nChans = 1;
			fi.sampRateCore = (int)sample_rate;
			AACSetRawBlockParams(enc->aac_dec, 0, &fi);
			RSS_DEBUG("rsp_audio: AAC decoder ready (raw mode, %uHz)", sample_rate);
		}
	}
#endif

	const char *codec_name = input_codec == RSP_CODEC_PCMU	 ? "G.711u"
				 : input_codec == RSP_CODEC_PCMA ? "G.711a"
				 : input_codec == RSP_CODEC_L16	 ? "L16"
				 : input_codec == RSP_CODEC_OPUS ? "Opus"
				 : input_codec == RSP_CODEC_AAC	 ? "AAC"
								 : "unknown";
	RSS_INFO("rsp_audio: %s @ %uHz -> AAC-LC @ %uHz (%d samples/frame)", codec_name,
		 sample_rate, enc->output_rate, enc->frame_samples);

	return enc;
}

/* Decode one ring frame to PCM. Returns sample count, 0 on failure. */
static int decode_to_pcm(rsp_audio_enc_t *enc, const uint8_t *data, uint32_t len, int16_t *pcm,
			 int pcm_max)
{
	int samples = 0;

	switch (enc->input_codec) {
	case RSP_CODEC_PCMU:
		samples = (int)len;
		if (samples > pcm_max)
			samples = pcm_max;
		for (int i = 0; i < samples; i++)
			pcm[i] = ulaw_table[data[i]];
		break;

	case RSP_CODEC_PCMA:
		samples = (int)len;
		if (samples > pcm_max)
			samples = pcm_max;
		for (int i = 0; i < samples; i++)
			pcm[i] = alaw_table[data[i]];
		break;

	case RSP_CODEC_L16: {
		const uint16_t *src = (const uint16_t *)data;
		samples = (int)(len / 2);
		if (samples > pcm_max)
			samples = pcm_max;
		for (int i = 0; i < samples; i++)
			pcm[i] = (int16_t)ntohs(src[i]);
		break;
	}

#ifdef RAPTOR_OPUS
	case RSP_CODEC_OPUS:
		if (!enc->opus_dec)
			return 0;
		samples = opus_decode(enc->opus_dec, data, (opus_int32)len, pcm, pcm_max, 0);
		if (samples < 0) {
			RSS_WARN("rsp_audio: opus decode error %d", samples);
			return 0;
		}
		break;
#endif

#ifdef RAPTOR_AAC
	case RSP_CODEC_AAC:
		if (!enc->aac_dec)
			return 0;
		{
			unsigned char *inp = (unsigned char *)data;
			int left = (int)len;
			int err = AACDecode(enc->aac_dec, &inp, &left, pcm);
			if (err != 0)
				return 0;
			AACFrameInfo info;
			AACGetLastFrameInfo(enc->aac_dec, &info);
			samples = info.outputSamps;
			if (info.nChans == 2) {
				samples /= 2;
				for (int i = 0; i < samples; i++)
					pcm[i] = (pcm[i * 2] + pcm[i * 2 + 1]) / 2;
			}
		}
		break;
#endif

	default:
		return 0;
	}

	return samples;
}

/*
 * Resample PCM from input_rate to output_rate using linear interpolation.
 * Returns number of output samples written.
 */
/*
 * Resample PCM from input_rate to output_rate.
 * For integer ratios (e.g. 16kHz→48kHz = 3:1), uses sample repetition
 * which is artifact-free — the AAC encoder's filterbank handles smoothing.
 * For non-integer ratios, falls back to linear interpolation.
 */
static int resample(rsp_audio_enc_t *enc, const int16_t *in, int in_count, int16_t *out,
		    int out_max)
{
	if (enc->input_rate == enc->output_rate) {
		int n = in_count < out_max ? in_count : out_max;
		memcpy(out, in, (size_t)n * sizeof(int16_t));
		return n;
	}

	/* Integer ratio: sample repetition (16→48 = 3x, 8→48 = 6x) */
	if (enc->output_rate % enc->input_rate == 0) {
		int ratio = (int)(enc->output_rate / enc->input_rate);
		int out_count = in_count * ratio;
		if (out_count > out_max)
			out_count = (out_max / ratio) * ratio;
		int n = out_count / ratio;
		for (int i = 0; i < n; i++) {
			for (int j = 0; j < ratio; j++)
				out[i * ratio + j] = in[i];
		}
		return out_count;
	}

	/* Non-integer ratio: linear interpolation */
	uint32_t step = ((uint32_t)enc->input_rate << 16) / enc->output_rate;
	uint32_t pos = enc->resample_frac;
	int out_count = 0;

	while (out_count < out_max) {
		uint32_t idx = pos >> 16;
		if (idx >= (uint32_t)in_count)
			break;
		uint32_t frac = (pos >> 8) & 0xFF;
		int32_t s0 = in[idx];
		int32_t s1 = (idx + 1 < (uint32_t)in_count) ? in[idx + 1] : s0;
		out[out_count++] = (int16_t)(s0 + ((s1 - s0) * (int32_t)frac >> 8));
		pos += step;
	}

	enc->resample_frac = pos - ((uint32_t)in_count << 16);
	return out_count;
}

int rsp_audio_transcode(rsp_audio_enc_t *enc, const uint8_t *data, uint32_t len,
			uint32_t timestamp_ms, rsp_audio_cb cb, void *userdata)
{
	/* Decode to PCM at input sample rate */
	int16_t pcm[4096];
	int samples = decode_to_pcm(enc, data, len, pcm, 4096);
	if (samples <= 0) {
		RSS_DEBUG("rsp_audio: decode failed (len=%u codec=%u)", len, enc->input_codec);
		return 0;
	}

	/* Resample to output rate (48kHz) */
	int16_t resampled[8192];
	int rs_count = resample(enc, pcm, samples, resampled, 8192);
	RSS_TRACE("rsp_audio: decode=%d resample=%d (in=%uHz out=%uHz)", samples, rs_count,
		  enc->input_rate, enc->output_rate);
	if (rs_count <= 0)
		return 0;

	/* Feed resampled PCM into faac accumulator.
	 * Smooth output timestamp: advances by frame_dur per AAC frame,
	 * with gentle correction toward ring timestamp to prevent drift
	 * without introducing jitter from accumulator boundary misalignment. */
	const int16_t *src = resampled;
	int remaining = rs_count;
	uint32_t frame_dur = (uint32_t)enc->frame_samples * 1000 / enc->output_rate;

	if (!enc->out_ts_running) {
		enc->next_out_ts = timestamp_ms;
		enc->out_ts_running = true;
	}

	while (remaining > 0) {
		int copy = remaining;
		if (enc->pcm_fill + copy > enc->frame_samples)
			copy = enc->frame_samples - enc->pcm_fill;
		memcpy(enc->pcm_buf + enc->pcm_fill, src, (size_t)copy * sizeof(int16_t));
		enc->pcm_fill += copy;
		src += copy;
		remaining -= copy;

		if (enc->pcm_fill >= enc->frame_samples) {
			int aac_len = faacEncEncode(enc->faac, (int32_t *)enc->pcm_buf,
						    enc->frame_samples, enc->aac_buf,
						    (unsigned int)enc->max_output);
			enc->pcm_fill = 0;
			if (aac_len > 0) {
				RSS_TRACE("rsp_audio: enc %d bytes ts=%u", aac_len,
					  enc->next_out_ts);
				if (cb(enc->aac_buf, (uint32_t)aac_len, enc->next_out_ts,
				       userdata) < 0)
					return -1;
				enc->next_out_ts += frame_dur;
				/* Correct toward ring timestamp to prevent drift.
				 * Snap on large gaps (overflow/reconnect), nudge
				 * on small drift (accumulator jitter). */
				int32_t err = (int32_t)timestamp_ms - (int32_t)enc->next_out_ts;
				if (err > (int32_t)frame_dur * 4 || err < -(int32_t)frame_dur * 4)
					enc->next_out_ts = timestamp_ms;
				else if (err > 1)
					enc->next_out_ts++;
				else if (err < -1)
					enc->next_out_ts--;
			}
		}
	}

	return 0;
}

void rsp_audio_reset(rsp_audio_enc_t *enc)
{
	if (!enc)
		return;
#ifdef RAPTOR_OPUS
	if (enc->opus_dec)
		opus_decoder_ctl(enc->opus_dec, OPUS_RESET_STATE);
#endif
#ifdef RAPTOR_AAC
	if (enc->aac_dec) {
		AACFlushCodec(enc->aac_dec);
	}
#endif
	enc->pcm_fill = 0;
	enc->out_ts_running = false;
}

void rsp_audio_free(rsp_audio_enc_t *enc)
{
	if (!enc)
		return;
	if (enc->faac)
		faacEncClose(enc->faac);
#ifdef RAPTOR_OPUS
	if (enc->opus_dec)
		opus_decoder_destroy(enc->opus_dec);
#endif
#ifdef RAPTOR_AAC
	if (enc->aac_dec)
		AACFreeDecoder(enc->aac_dec);
#endif
	free(enc->pcm_buf);
	free(enc->aac_buf);
	free(enc);
}

#endif /* RAPTOR_AAC_ENC */

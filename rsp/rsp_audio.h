/*
 * rsp_audio.h -- Audio transcode for RTMP push
 *
 * Decodes any ring audio codec (G.711, L16, Opus, AAC) to PCM,
 * then encodes to AAC-LC via faac for RTMP/FLV.
 */

#ifndef RSP_AUDIO_H
#define RSP_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

/* Ring codec IDs (match rss_ipc / RMR) */
#define RSP_CODEC_PCMU 0
#define RSP_CODEC_PCMA 8
#define RSP_CODEC_L16  11
#define RSP_CODEC_AAC  97
#define RSP_CODEC_OPUS 111

typedef struct rsp_audio_enc rsp_audio_enc_t;

/*
 * Initialize the audio transcoder.
 * input_codec: ring codec ID
 * sample_rate: ring sample rate (e.g. 16000)
 * Returns context on success, NULL on failure.
 */
rsp_audio_enc_t *rsp_audio_init(uint32_t input_codec, uint32_t sample_rate);

/*
 * Transcode one ring audio frame to AAC.
 * Input: raw ring frame (G.711/L16/Opus/AAC).
 * Output: zero or more AAC frames via callback (faac needs 1024 samples).
 *
 * The callback is called for each complete AAC frame produced.
 * Returns 0 on success, -1 on error.
 */
typedef int (*rsp_audio_cb)(const uint8_t *aac_data, uint32_t aac_len, uint32_t timestamp_ms,
			    void *userdata);

int rsp_audio_transcode(rsp_audio_enc_t *enc, const uint8_t *data, uint32_t len,
			uint32_t timestamp_ms, rsp_audio_cb cb, void *userdata);

/*
 * Reset decoder state after ring overflow (prevents clicks from
 * stale Opus/AAC decoder prediction state after frame gaps).
 */
void rsp_audio_reset(rsp_audio_enc_t *enc);

/*
 * Free the transcoder.
 */
void rsp_audio_free(rsp_audio_enc_t *enc);

/*
 * Returns true if audio needs transcoding.
 * RTMP servers (YouTube/Twitch) require AAC at 44.1 or 48kHz.
 * Transcode if codec is not AAC, or if sample rate is non-standard.
 */
static inline bool rsp_audio_needs_transcode(uint32_t codec, uint32_t sample_rate)
{
	if (codec != RSP_CODEC_AAC)
		return true;
	return (sample_rate != 44100 && sample_rate != 48000);
}

#endif /* RSP_AUDIO_H */

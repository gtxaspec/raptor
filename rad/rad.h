/*
 * rad.h -- Raptor Audio Daemon internal state
 */

#ifndef RAD_H
#define RAD_H

#include <stdint.h>
#include <stdbool.h>
#include <rss_common.h>
#include <rss_ipc.h>

/* Audio codec IDs stored in ring stream_info (RTP payload types) */
#define RAD_CODEC_PCMU 0
#define RAD_CODEC_PCMA 8
#define RAD_CODEC_L16  11
#define RAD_CODEC_AAC  97
#define RAD_CODEC_OPUS 111

/*
 * Codec plugin interface.
 *
 * Each codec provides init/encode/deinit. The encode function receives
 * PCM samples and writes encoded output to the provided buffer. For
 * codecs that accumulate frames (e.g., AAC needs 1024 samples but HAL
 * delivers 320), encode may output 0 bytes and publish internally.
 *
 * Codec-specific state is stored in ctx->priv (allocated by init,
 * freed by deinit).
 */
typedef struct rad_codec_ctx {
	void *priv;	     /* codec-private state */
	rss_ring_t *ring;    /* ring for direct publish (AAC accumulation) */
	int codec_id;	     /* RAD_CODEC_* */
	int encode_buf_size; /* minimum encode output buffer size */
} rad_codec_ctx_t;

typedef struct {
	const char *name; /* config name: "pcmu", "pcma", "l16", "aac", "opus" */
	int codec_id;	  /* RAD_CODEC_* */

	/* Initialize codec. Returns 0 on success. */
	int (*init)(rad_codec_ctx_t *ctx, rss_config_t *cfg, int sample_rate);

	/* Encode PCM samples. Returns bytes written to out, or 0 if
	 * the codec accumulated internally (AAC). Negative on error. */
	int (*encode)(rad_codec_ctx_t *ctx, const int16_t *pcm, int samples, uint8_t *out,
		      int out_size, int64_t timestamp);

	/* Clean up codec state. */
	void (*deinit)(rad_codec_ctx_t *ctx);
} rad_codec_ops_t;

/* Codec registry — returns ops for a given config name, or NULL */
const rad_codec_ops_t *rad_codec_find(const char *name);

/* Built-in codecs */
extern const rad_codec_ops_t rad_codec_pcmu;
extern const rad_codec_ops_t rad_codec_pcma;
extern const rad_codec_ops_t rad_codec_l16;
#ifdef RAPTOR_AAC
extern const rad_codec_ops_t rad_codec_aac;
#endif
#ifdef RAPTOR_OPUS
extern const rad_codec_ops_t rad_codec_opus;
#endif

#endif /* RAD_H */

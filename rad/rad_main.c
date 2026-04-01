/*
 * rad_main.c -- Raptor Audio Daemon
 *
 * Captures PCM audio from the ISP's audio input, optionally encodes,
 * and publishes to SHM ring buffer "audio".
 *
 * Supported codecs (config: [audio] codec):
 *   pcmu  — G.711 mu-law, 8kHz fixed (RTP PT 0)
 *   pcma  — G.711 A-law, 8kHz fixed (RTP PT 8)
 *   l16   — Uncompressed 16-bit PCM, any sample rate (RTP PT dynamic)
 *   aac   — AAC-LC via faac (compile with RAPTOR_AAC=1)
 *   opus  — Opus via libopus (compile with RAPTOR_OPUS=1)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>

#include <pthread.h>
#include <sys/select.h>

#include <raptor_hal.h>
#include <rss_ipc.h>
#include <rss_common.h>

#ifdef RAPTOR_AAC
#include <faac.h>
#endif

#ifdef RAPTOR_OPUS
#include <opus/opus.h>
#endif

/* Audio codec IDs stored in ring stream_info (RTP payload types) */
#define RAD_CODEC_PCMU 0
#define RAD_CODEC_PCMA 8
#define RAD_CODEC_L16  11
#define RAD_CODEC_AAC  97
#define RAD_CODEC_OPUS 111

/* ── G.711 mu-law encoding ── */

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

/* ── G.711 A-law encoding ── */

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

/* ── L16 encoding (PCM16 to network byte order) ── */

static void pcm16_to_l16(const int16_t *pcm, uint8_t *out, int samples)
{
	uint16_t *dst = (uint16_t *)out;
	for (int i = 0; i < samples; i++)
		dst[i] = htons((uint16_t)pcm[i]);
}

/* ── Control socket handler ── */

typedef struct {
	rss_config_t *cfg;
	const char *config_path;
	const rss_hal_ops_t *ops;
	rss_hal_ctx_t *hal_ctx;
	int ai_dev;
	int volume;
	int gain;
	int sample_rate;
	int codec_id;
	const char *codec_str;
	bool ao_enabled;
	int ao_volume;
	int ao_gain;
#ifdef RAPTOR_AUDIO_EFFECTS
	bool ns_enabled;
	bool hpf_enabled;
	bool agc_enabled;
#endif
} rad_ctrl_ctx_t;

static void rad_fmt_result(char *buf, int bufsz, int ret)
{
	if (ret == 0)
		snprintf(buf, bufsz, "{\"status\":\"ok\"}");
	else if (ret == RSS_ERR_NOTSUP)
		snprintf(buf, bufsz, "{\"status\":\"error\",\"reason\":\"not supported on this SoC\"}");
	else
		snprintf(buf, bufsz, "{\"status\":\"error\",\"reason\":\"failed (%d)\"}", ret);
}

#define CTRL_RESP(buf) return (int)strlen(buf)

static int rad_ctrl_handler(const char *cmd_json, char *resp_buf, int resp_buf_size, void *userdata)
{
	rad_ctrl_ctx_t *ctx = userdata;
	int val;

	int rc = rss_ctrl_handle_common(cmd_json, resp_buf, resp_buf_size, ctx->cfg, ctx->config_path);
	if (rc >= 0)
		return rc;

	if (strstr(cmd_json, "\"set-volume\"")) {
		if (rss_json_get_int(cmd_json, "value", &val) == 0) {
			int ret = RSS_HAL_CALL(ctx->ops, audio_set_volume, ctx->hal_ctx, ctx->ai_dev, 0, val);
			if (ret == 0) {
				ctx->volume = val;
				rss_config_set_int(ctx->cfg, "audio", "volume", val);
			}
			rad_fmt_result(resp_buf, resp_buf_size, ret);
		} else {
			snprintf(resp_buf, resp_buf_size,
				 "{\"status\":\"error\",\"reason\":\"need value\"}");
		}
		CTRL_RESP(resp_buf);
	}

	if (strstr(cmd_json, "\"set-gain\"")) {
		if (rss_json_get_int(cmd_json, "value", &val) == 0) {
			int ret = RSS_HAL_CALL(ctx->ops, audio_set_gain, ctx->hal_ctx, ctx->ai_dev, 0, val);
			if (ret == 0) {
				ctx->gain = val;
				rss_config_set_int(ctx->cfg, "audio", "gain", val);
			}
			rad_fmt_result(resp_buf, resp_buf_size, ret);
		} else {
			snprintf(resp_buf, resp_buf_size,
				 "{\"status\":\"error\",\"reason\":\"need value\"}");
		}
		CTRL_RESP(resp_buf);
	}

	if (strstr(cmd_json, "\"set-alc-gain\"")) {
		if (rss_json_get_int(cmd_json, "value", &val) == 0) {
			int ret = RSS_HAL_CALL(ctx->ops, audio_set_alc_gain, ctx->hal_ctx,
					       ctx->ai_dev, 0, val);
			if (ret == 0) {
				rss_config_set_int(ctx->cfg, "audio", "alc_gain", val);
				snprintf(resp_buf, resp_buf_size, "{\"status\":\"ok\"}");
			} else {
				snprintf(resp_buf, resp_buf_size,
					 "{\"status\":\"error\",\"reason\":\"%s\"}",
					 ret == RSS_ERR_NOTSUP ? "not supported on this platform"
								: "failed");
			}
		} else {
			snprintf(resp_buf, resp_buf_size,
				 "{\"status\":\"error\",\"reason\":\"need value 0-7\"}");
		}
		CTRL_RESP(resp_buf);
	}

#ifdef RAPTOR_AUDIO_EFFECTS
	if (strstr(cmd_json, "\"set-ns\"")) {
		char level[16] = "";
		rss_json_get_str(cmd_json, "level", level, sizeof(level));
		if (rss_json_get_int(cmd_json, "value", &val) == 0) {
			int ret = RSS_OK;
			if (val && !ctx->ns_enabled) {
				rss_ns_level_t ns = RSS_NS_MODERATE;
				if (strcasecmp(level, "low") == 0)
					ns = RSS_NS_LOW;
				else if (strcasecmp(level, "high") == 0)
					ns = RSS_NS_HIGH;
				else if (strcasecmp(level, "veryhigh") == 0)
					ns = RSS_NS_VERYHIGH;
				ret = RSS_HAL_CALL(ctx->ops, audio_enable_ns, ctx->hal_ctx, ns);
			} else if (!val && ctx->ns_enabled) {
				ret = RSS_HAL_CALL(ctx->ops, audio_disable_ns, ctx->hal_ctx);
			}
			if (ret == RSS_OK)
				ctx->ns_enabled = !!val;
			if (ret == RSS_ERR_NOTSUP)
				snprintf(resp_buf, resp_buf_size,
					 "{\"status\":\"error\",\"reason\":\"not supported on this SoC\"}");
			else
				snprintf(resp_buf, resp_buf_size, "{\"status\":\"%s\",\"ns\":%s}",
					 ret == RSS_OK ? "ok" : "error",
					 ctx->ns_enabled ? "true" : "false");
		} else {
			snprintf(resp_buf, resp_buf_size,
				 "{\"status\":\"error\",\"reason\":\"need value (0/1)\"}");
		}
		CTRL_RESP(resp_buf);
	}

	if (strstr(cmd_json, "\"set-hpf\"")) {
		if (rss_json_get_int(cmd_json, "value", &val) == 0) {
			int ret = RSS_OK;
			if (val && !ctx->hpf_enabled)
				ret = RSS_HAL_CALL(ctx->ops, audio_enable_hpf, ctx->hal_ctx);
			else if (!val && ctx->hpf_enabled)
				ret = RSS_HAL_CALL(ctx->ops, audio_disable_hpf, ctx->hal_ctx);
			if (ret == RSS_OK)
				ctx->hpf_enabled = !!val;
			if (ret == RSS_ERR_NOTSUP)
				snprintf(resp_buf, resp_buf_size,
					 "{\"status\":\"error\",\"reason\":\"not supported on this SoC\"}");
			else
				snprintf(resp_buf, resp_buf_size, "{\"status\":\"%s\",\"hpf\":%s}",
					 ret == RSS_OK ? "ok" : "error",
					 ctx->hpf_enabled ? "true" : "false");
		} else {
			snprintf(resp_buf, resp_buf_size,
				 "{\"status\":\"error\",\"reason\":\"need value (0/1)\"}");
		}
		CTRL_RESP(resp_buf);
	}

	if (strstr(cmd_json, "\"set-agc\"")) {
		if (rss_json_get_int(cmd_json, "value", &val) == 0) {
			int ret = RSS_OK;
			if (val) {
				int target = 10, compression = 0;
				rss_json_get_int(cmd_json, "target", &target);
				rss_json_get_int(cmd_json, "compression", &compression);
				rss_agc_config_t agc_cfg = {
					.target_level_dbfs = target,
					.compression_gain_db = compression,
				};
				ret = RSS_HAL_CALL(ctx->ops, audio_enable_agc, ctx->hal_ctx,
						   &agc_cfg);
			} else if (ctx->agc_enabled) {
				ret = RSS_HAL_CALL(ctx->ops, audio_disable_agc, ctx->hal_ctx);
			}
			if (ret == RSS_OK)
				ctx->agc_enabled = !!val;
			if (ret == RSS_ERR_NOTSUP)
				snprintf(resp_buf, resp_buf_size,
					 "{\"status\":\"error\",\"reason\":\"not supported on this SoC\"}");
			else
				snprintf(resp_buf, resp_buf_size, "{\"status\":\"%s\",\"agc\":%s}",
					 ret == RSS_OK ? "ok" : "error",
					 ctx->agc_enabled ? "true" : "false");
		} else {
			snprintf(resp_buf, resp_buf_size,
				 "{\"status\":\"error\",\"reason\":\"need value (0/1)\"}");
		}
		CTRL_RESP(resp_buf);
	}
#endif /* RAPTOR_AUDIO_EFFECTS */

	if (strstr(cmd_json, "\"ao-set-volume\"")) {
		if (rss_json_get_int(cmd_json, "value", &val) == 0 && ctx->ao_enabled) {
			RSS_HAL_CALL(ctx->ops, ao_set_volume, ctx->hal_ctx, val);
			ctx->ao_volume = val;
			snprintf(resp_buf, resp_buf_size, "{\"status\":\"ok\"}");
		} else {
			snprintf(resp_buf, resp_buf_size,
				 "{\"status\":\"error\",\"reason\":\"%s\"}",
				 ctx->ao_enabled ? "need value" : "ao disabled");
		}
		CTRL_RESP(resp_buf);
	}

	if (strstr(cmd_json, "\"ao-set-gain\"")) {
		if (rss_json_get_int(cmd_json, "value", &val) == 0 && ctx->ao_enabled) {
			RSS_HAL_CALL(ctx->ops, ao_set_gain, ctx->hal_ctx, val);
			ctx->ao_gain = val;
			snprintf(resp_buf, resp_buf_size, "{\"status\":\"ok\"}");
		} else {
			snprintf(resp_buf, resp_buf_size,
				 "{\"status\":\"error\",\"reason\":\"%s\"}",
				 ctx->ao_enabled ? "need value" : "ao disabled");
		}
		CTRL_RESP(resp_buf);
	}

	if (strstr(cmd_json, "\"config-show\"")) {
		snprintf(resp_buf, resp_buf_size,
			 "{\"status\":\"ok\",\"config\":{"
			 "\"codec\":\"%s\",\"sample_rate\":%d,"
			 "\"volume\":%d,\"gain\":%d,\"device\":%d,"
			 "\"config_path\":\"%s\"}}",
			 ctx->codec_str, ctx->sample_rate, ctx->volume, ctx->gain, ctx->ai_dev,
			 ctx->config_path);
		CTRL_RESP(resp_buf);
	}

	if (strstr(cmd_json, "\"status\"")) {
		int n = snprintf(resp_buf, resp_buf_size,
				 "{\"status\":\"ok\",\"codec\":\"%s\","
				 "\"sample_rate\":%d,\"volume\":%d,\"gain\":%d,"
				 "\"ao_enabled\":%s",
				 ctx->codec_str, ctx->sample_rate, ctx->volume, ctx->gain,
				 ctx->ao_enabled ? "true" : "false");
		if (ctx->ao_enabled)
			n += snprintf(resp_buf + n, resp_buf_size - n,
				      ",\"ao_volume\":%d,\"ao_gain\":%d", ctx->ao_volume,
				      ctx->ao_gain);
#ifdef RAPTOR_AUDIO_EFFECTS
		n += snprintf(resp_buf + n, resp_buf_size - n, ",\"ns\":%s,\"hpf\":%s,\"agc\":%s",
			      ctx->ns_enabled ? "true" : "false",
			      ctx->hpf_enabled ? "true" : "false",
			      ctx->agc_enabled ? "true" : "false");
#endif
		snprintf(resp_buf + n, resp_buf_size - n, "}");
		CTRL_RESP(resp_buf);
	}

	snprintf(resp_buf, resp_buf_size, "{\"status\":\"error\",\"reason\":\"unknown command\"}");
	CTRL_RESP(resp_buf);
}

/* ── AO playback thread: reads from speaker ring, sends to hardware ── */

typedef struct {
	const rss_hal_ops_t *ops;
	rss_hal_ctx_t *hal_ctx;
	volatile sig_atomic_t *running;
} ao_thread_ctx_t;

static void *ao_playback_thread(void *arg)
{
	ao_thread_ctx_t *ctx = arg;

	RSS_INFO("ao playback thread started, waiting for speaker ring...");

	/* Poll for speaker ring (created by rac play) */
	rss_ring_t *ring = NULL;
	while (*ctx->running) {
		ring = rss_ring_open("speaker");
		if (ring)
			break;
		usleep(500000);
	}
	if (!ring) {
		RSS_INFO("ao playback thread exiting (no ring)");
		return NULL;
	}

	const rss_ring_header_t *hdr = rss_ring_get_header(ring);
	uint8_t *buf = malloc(hdr->data_size);
	if (!buf) {
		RSS_FATAL("ao thread: malloc failed");
		rss_ring_close(ring);
		return NULL;
	}

	uint64_t read_seq = 0;
	uint64_t last_write_seq = atomic_load(&hdr->write_seq);
	int idle_count = 0;
	RSS_INFO("speaker ring connected");

	while (*ctx->running) {
		int ret = rss_ring_wait(ring, 200);
		if (ret != 0) {
			uint64_t ws = atomic_load(&hdr->write_seq);
			if (ws == last_write_seq)
				idle_count++;
			else
				idle_count = 0;
			last_write_seq = ws;

			/* After ~1s of no new data, close and wait for fresh ring */
			if (idle_count >= 5) {
				idle_count = 0;
				free(buf);
				rss_ring_close(ring);
				ring = NULL;
				buf = NULL;
				RSS_DEBUG("speaker idle, waiting for new ring...");
				while (*ctx->running) {
					ring = rss_ring_open("speaker");
					if (ring) {
						hdr = rss_ring_get_header(ring);
						buf = malloc(hdr->data_size);
						if (!buf) {
							rss_ring_close(ring);
							ring = NULL;
							break;
						}
						read_seq = 0;
						last_write_seq = atomic_load(&hdr->write_seq);
						RSS_INFO("speaker ring reconnected");
						break;
					}
					usleep(100000);
				}
				if (!ring || !buf)
					break;
			}
			continue;
		}
		idle_count = 0;

		uint32_t length = 0;
		rss_ring_slot_t meta;
		ret = rss_ring_read(ring, &read_seq, buf, hdr->data_size, &length, &meta);
		if (ret != 0) {
			RSS_DEBUG("ao ring_read failed: %d seq=%llu", ret,
				  (unsigned long long)read_seq);
			continue;
		}

		RSS_DEBUG("ao send_frame len=%u", length);
		RSS_HAL_CALL(ctx->ops, ao_send_frame, ctx->hal_ctx, (const int16_t *)buf, length,
			     true);
	}

	free(buf);
	if (ring)
		rss_ring_close(ring);
	RSS_INFO("ao playback thread exiting");
	return NULL;
}

int main(int argc, char **argv)
{
	rss_daemon_ctx_t dctx;
	int ret = rss_daemon_init(&dctx, "rad", argc, argv);
	if (ret != 0)
		return ret < 0 ? 1 : 0;
	RSS_BANNER("rad");

	if (!rss_config_get_bool(dctx.cfg, "audio", "enabled", true)) {
		RSS_INFO("audio disabled in config");
		rss_config_free(dctx.cfg);
		rss_daemon_cleanup("rad");
		return 0;
	}

	rss_ctrl_t *ctrl = NULL;
	rss_ring_t *ring = NULL;
	uint8_t *encode_buf = NULL;
	int16_t *aac_pcm_buf = NULL;
#ifdef RAPTOR_AAC
	faacEncHandle aac_handle = NULL;
#endif
#ifdef RAPTOR_OPUS
	OpusEncoder *opus_enc = NULL;
#endif
	bool ao_enabled = false;
	pthread_t ao_tid;
	bool ao_thread_started = false;
	ao_thread_ctx_t ao_ctx = {0};
	const rss_hal_ops_t *ops = NULL;

	rss_hal_ctx_t *hal_ctx = rss_hal_create();
	if (!hal_ctx) {
		RSS_FATAL("rss_hal_create failed");
		goto cleanup;
	}
	ops = rss_hal_get_ops(hal_ctx);

	/* Audio config */
	int sample_rate = rss_config_get_int(dctx.cfg, "audio", "sample_rate", 16000);
	int volume = rss_config_get_int(dctx.cfg, "audio", "volume", 80);
	int gain = rss_config_get_int(dctx.cfg, "audio", "gain", 25);
	int ai_dev = rss_config_get_int(dctx.cfg, "audio", "device", 1);
	const char *codec_str = rss_config_get_str(dctx.cfg, "audio", "codec", "l16");

	/* Parse codec */
	int codec_id;
	if (strcasecmp(codec_str, "pcmu") == 0) {
		codec_id = RAD_CODEC_PCMU;
		sample_rate = 8000; /* PCMU is 8kHz only */
	} else if (strcasecmp(codec_str, "pcma") == 0) {
		codec_id = RAD_CODEC_PCMA;
		sample_rate = 8000; /* PCMA is 8kHz only */
	} else if (strcasecmp(codec_str, "l16") == 0) {
		codec_id = RAD_CODEC_L16;
#ifdef RAPTOR_AAC
	} else if (strcasecmp(codec_str, "aac") == 0) {
		codec_id = RAD_CODEC_AAC;
#endif
#ifdef RAPTOR_OPUS
	} else if (strcasecmp(codec_str, "opus") == 0) {
		codec_id = RAD_CODEC_OPUS;
#endif
	} else {
		RSS_FATAL("unknown audio codec: %s (supported: pcmu, pcma, l16"
#ifdef RAPTOR_AAC
			  ", aac"
#endif
#ifdef RAPTOR_OPUS
			  ", opus"
#endif
			  ")",
			  codec_str);
		goto cleanup;
	}

	rss_audio_config_t audio_cfg = {
		.sample_rate = sample_rate,
		.samples_per_frame = sample_rate / 50, /* 20ms frames */
		.chn_count = 1,
		.frame_depth = 20,
		.ai_vol = volume,
		.ai_gain = gain,
	};

	ret = RSS_HAL_CALL(ops, audio_init, hal_ctx, &audio_cfg);
	if (ret != RSS_OK) {
		RSS_FATAL("audio_init failed: %d", ret);
		goto cleanup;
	}

	RSS_HAL_CALL(ops, audio_set_volume, hal_ctx, ai_dev, 0, volume);
	RSS_HAL_CALL(ops, audio_set_gain, hal_ctx, ai_dev, 0, gain);

	RSS_INFO("audio: dev=%d %d Hz %s vol=%d gain=%d", ai_dev, sample_rate, codec_str, volume,
		 gain);

	/* ── Encoder init (AAC/Opus) ── */
#ifdef RAPTOR_AAC
	unsigned long aac_input_samples = 0;
	unsigned long aac_max_output = 0;
#endif

#ifdef RAPTOR_AAC
	if (codec_id == RAD_CODEC_AAC) {
		aac_handle = faacEncOpen(sample_rate, 1, &aac_input_samples, &aac_max_output);
		if (!aac_handle) {
			RSS_FATAL("faacEncOpen failed");
			goto cleanup;
		}
		faacEncConfigurationPtr aac_cfg = faacEncGetCurrentConfiguration(aac_handle);
		aac_cfg->aacObjectType = LOW;
		aac_cfg->mpegVersion = MPEG4;
		aac_cfg->inputFormat = FAAC_INPUT_16BIT;
		aac_cfg->outputFormat = RAW_STREAM;
		aac_cfg->bandWidth = sample_rate;
		int aac_br = rss_config_get_int(dctx.cfg, "audio", "bitrate", 32000);
		if (aac_br > 256000)
			aac_br = 256000;
		aac_cfg->bitRate = aac_br; /* per channel */
		aac_cfg->allowMidside = 0;
		aac_cfg->useTns = 0;
		if (!faacEncSetConfiguration(aac_handle, aac_cfg)) {
			RSS_FATAL("faacEncSetConfiguration failed");
			goto cleanup;
		}
		RSS_INFO("aac encoder: %lu samples/frame, max %lu bytes output", aac_input_samples,
			 aac_max_output);
	}
#endif

#ifdef RAPTOR_OPUS
	if (codec_id == RAD_CODEC_OPUS) {
		int opus_err;
		opus_enc = opus_encoder_create(sample_rate, 1, OPUS_APPLICATION_RESTRICTED_LOWDELAY,
					       &opus_err);
		if (opus_err != OPUS_OK || !opus_enc) {
			RSS_FATAL("opus_encoder_create failed: %d", opus_err);
			goto cleanup;
		}
		int opus_bitrate = rss_config_get_int(dctx.cfg, "audio", "bitrate", 32000);
		if (opus_bitrate > 256000)
			opus_bitrate = 256000;
		opus_encoder_ctl(opus_enc, OPUS_SET_BITRATE(opus_bitrate));
		RSS_INFO("opus encoder: bitrate=%d", opus_bitrate);
	}
#endif

	/* ── Audio effects (libaudioProcess.so) ── */
#ifdef RAPTOR_AUDIO_EFFECTS
	bool ns_enabled = rss_config_get_bool(dctx.cfg, "audio", "ns_enabled", false);
	if (ns_enabled) {
		const char *ns_str = rss_config_get_str(dctx.cfg, "audio", "ns_level", "moderate");
		rss_ns_level_t ns_level = RSS_NS_MODERATE;
		if (strcasecmp(ns_str, "low") == 0)
			ns_level = RSS_NS_LOW;
		else if (strcasecmp(ns_str, "high") == 0)
			ns_level = RSS_NS_HIGH;
		else if (strcasecmp(ns_str, "veryhigh") == 0)
			ns_level = RSS_NS_VERYHIGH;
		ret = RSS_HAL_CALL(ops, audio_enable_ns, hal_ctx, ns_level);
		if (ret == RSS_OK)
			RSS_INFO("noise suppression: %s", ns_str);
		else
			RSS_WARN("noise suppression failed: %d", ret);
	}

	bool hpf_enabled = rss_config_get_bool(dctx.cfg, "audio", "hpf_enabled", false);
	if (hpf_enabled) {
		ret = RSS_HAL_CALL(ops, audio_enable_hpf, hal_ctx);
		if (ret == RSS_OK)
			RSS_INFO("high-pass filter enabled");
		else
			RSS_WARN("high-pass filter failed: %d", ret);
	}

	bool agc_enabled = rss_config_get_bool(dctx.cfg, "audio", "agc_enabled", false);
	if (agc_enabled) {
		rss_agc_config_t agc_cfg = {
			.target_level_dbfs =
				rss_config_get_int(dctx.cfg, "audio", "agc_target_dbfs", 10),
			.compression_gain_db =
				rss_config_get_int(dctx.cfg, "audio", "agc_compression_db", 0),
		};
		ret = RSS_HAL_CALL(ops, audio_enable_agc, hal_ctx, &agc_cfg);
		if (ret == RSS_OK)
			RSS_INFO("agc: target=%d dBfs, compression=%d dB",
				 agc_cfg.target_level_dbfs, agc_cfg.compression_gain_db);
		else
			RSS_WARN("agc failed: %d", ret);
	}
#endif

	/* ── Audio output (speaker) ── */
	ao_enabled = rss_config_get_bool(dctx.cfg, "audio", "ao_enabled", false);

	if (ao_enabled) {
		rss_audio_config_t ao_cfg = {
			.sample_rate = sample_rate,
			.samples_per_frame = sample_rate / 50,
			.chn_count = 1,
			.frame_depth = 20,
		};
		ret = RSS_HAL_CALL(ops, ao_init, hal_ctx, &ao_cfg);
		if (ret != RSS_OK) {
			RSS_WARN("ao_init failed: %d (speaker disabled)", ret);
			ao_enabled = false;
		} else {
			int ao_vol = rss_config_get_int(dctx.cfg, "audio", "ao_volume", 80);
			int ao_gain_val = rss_config_get_int(dctx.cfg, "audio", "ao_gain", 25);
			RSS_HAL_CALL(ops, ao_set_volume, hal_ctx, ao_vol);
			RSS_HAL_CALL(ops, ao_set_gain, hal_ctx, ao_gain_val);

			ao_ctx = (ao_thread_ctx_t){
				.ops = ops,
				.hal_ctx = hal_ctx,
				.running = dctx.running,
			};
			if (pthread_create(&ao_tid, NULL, ao_playback_thread, &ao_ctx) == 0) {
				ao_thread_started = true;
				RSS_INFO("audio output: %d Hz vol=%d gain=%d", sample_rate, ao_vol,
					 ao_gain_val);
			} else {
				RSS_WARN("ao thread create failed");
				ao_enabled = false;
			}
		}
	}

	/* Ring buffer */
	int ring_data_size = (codec_id == RAD_CODEC_L16) ? 256 * 1024 : 128 * 1024;
	ring = rss_ring_create("audio", 32, ring_data_size);
	if (!ring) {
		RSS_FATAL("failed to create audio ring");
		goto cleanup;
	}
	rss_ring_set_stream_info(ring, 0x10, codec_id, 0, 0, sample_rate, 1, 0, 0);

	/* Encode buffer — sized for worst case of each codec */
	int encode_buf_size;
	if (codec_id == RAD_CODEC_L16)
		encode_buf_size = audio_cfg.samples_per_frame * 2;
#ifdef RAPTOR_AAC
	else if (codec_id == RAD_CODEC_AAC)
		encode_buf_size = (int)(aac_max_output > 0 ? aac_max_output : 4096);
#endif
#ifdef RAPTOR_OPUS
	else if (codec_id == RAD_CODEC_OPUS)
		encode_buf_size = 4096;
#endif
	else
		encode_buf_size = audio_cfg.samples_per_frame; /* G.711: 1 byte/sample */
	encode_buf = malloc(encode_buf_size);
	if (!encode_buf) {
		RSS_FATAL("failed to allocate encode buffer");
		goto cleanup;
	}

	/* Control socket */
	rss_mkdir_p("/var/run/rss");
	ctrl = rss_ctrl_listen("/var/run/rss/rad.sock");
	if (!ctrl)
		RSS_WARN("control socket failed (non-fatal)");

	/* AAC accumulation buffer — faac needs aac_input_samples (typically 1024)
	 * but HAL delivers samples_per_frame (typically 320) per capture.
	 * Buffer captures until we have enough for one full AAC frame. */
#ifdef RAPTOR_AAC
	int aac_pcm_fill = 0;
	int aac_frame_samples = 0;
	if (codec_id == RAD_CODEC_AAC) {
		aac_frame_samples = (int)aac_input_samples;
		aac_pcm_buf = malloc(aac_frame_samples * sizeof(int16_t));
		if (!aac_pcm_buf) {
			RSS_FATAL("aac pcm buffer alloc failed");
			goto cleanup;
		}
	}
#endif

	RSS_INFO("audio loop: %d samples/frame (%dms), %s", audio_cfg.samples_per_frame, 1000 / 50,
		 codec_str);

	rad_ctrl_ctx_t ctrl_ctx = {
		.cfg = dctx.cfg,
		.config_path = dctx.config_path,
		.ops = ops,
		.hal_ctx = hal_ctx,
		.ai_dev = ai_dev,
		.volume = volume,
		.gain = gain,
		.sample_rate = sample_rate,
		.codec_id = codec_id,
		.codec_str = codec_str,
		.ao_enabled = ao_enabled,
		.ao_volume = rss_config_get_int(dctx.cfg, "audio", "ao_volume", 80),
		.ao_gain = rss_config_get_int(dctx.cfg, "audio", "ao_gain", 25),
#ifdef RAPTOR_AUDIO_EFFECTS
		.ns_enabled = ns_enabled,
		.hpf_enabled = hpf_enabled,
		.agc_enabled = agc_enabled,
#endif
	};

	uint64_t frame_count = 0;
	int64_t last_stats = rss_timestamp_us();

	while (*dctx.running) {
		/* Check control socket (non-blocking) */
		if (ctrl) {
			int ctrl_fd = rss_ctrl_get_fd(ctrl);
			if (ctrl_fd >= 0) {
				fd_set fds;
				struct timeval tv = {0, 0};
				FD_ZERO(&fds);
				FD_SET(ctrl_fd, &fds);
				if (select(ctrl_fd + 1, &fds, NULL, NULL, &tv) > 0)
					rss_ctrl_accept_and_handle(ctrl, rad_ctrl_handler,
								   &ctrl_ctx);
			}
		}

		rss_audio_frame_t frame;
		ret = RSS_HAL_CALL(ops, audio_read_frame, hal_ctx, ai_dev, 0, &frame, true);
		if (ret != RSS_OK) {
			if (ret == RSS_ERR_TIMEOUT)
				continue;
			RSS_WARN("audio_read_frame failed: %d", ret);
			usleep(10000);
			continue;
		}

		int samples = frame.length / 2;
		if (samples > (int)audio_cfg.samples_per_frame)
			samples = audio_cfg.samples_per_frame;

		const int16_t *pcm = frame.data;
		int out_len;

		switch (codec_id) {
		case RAD_CODEC_PCMU:
			for (int i = 0; i < samples; i++)
				encode_buf[i] = pcm16_to_ulaw(pcm[i]);
			out_len = samples;
			break;
		case RAD_CODEC_PCMA:
			for (int i = 0; i < samples; i++)
				encode_buf[i] = pcm16_to_alaw(pcm[i]);
			out_len = samples;
			break;
		case RAD_CODEC_L16:
			pcm16_to_l16(pcm, encode_buf, samples);
			out_len = samples * 2;
			break;
#ifdef RAPTOR_AAC
		case RAD_CODEC_AAC: {
			/* Accumulate PCM until we have aac_frame_samples (1024).
			 * Carry over leftover samples from each capture. */
			const int16_t *src = pcm;
			int remaining = samples;
			out_len = 0;
			while (remaining > 0) {
				int copy = remaining;
				if (aac_pcm_fill + copy > aac_frame_samples)
					copy = aac_frame_samples - aac_pcm_fill;
				memcpy(aac_pcm_buf + aac_pcm_fill, src, copy * sizeof(int16_t));
				aac_pcm_fill += copy;
				src += copy;
				remaining -= copy;
				if (aac_pcm_fill >= aac_frame_samples) {
					out_len = faacEncEncode(aac_handle, (int32_t *)aac_pcm_buf,
								aac_frame_samples, encode_buf,
								encode_buf_size);
					if (out_len < 0) {
						RSS_WARN("aac encode failed: %d", out_len);
						out_len = 0;
					}
					aac_pcm_fill = 0;
					if (out_len > 0)
						rss_ring_publish(ring, encode_buf, out_len,
								 frame.timestamp, codec_id, 0);
				}
			}
			/* Skip the publish below — handled inside the loop */
			out_len = 0;
			break;
		}
#endif
#ifdef RAPTOR_OPUS
		case RAD_CODEC_OPUS:
			out_len = opus_encode(opus_enc, pcm, samples, encode_buf, encode_buf_size);
			if (out_len < 0) {
				RSS_WARN("opus encode failed: %d", out_len);
				out_len = 0;
			}
			break;
#endif
		default:
			out_len = 0;
			break;
		}

		if (out_len > 0)
			rss_ring_publish(ring, encode_buf, out_len, frame.timestamp, codec_id, 0);

		RSS_HAL_CALL(ops, audio_release_frame, hal_ctx, ai_dev, 0, &frame);

		frame_count++;

		int64_t now = rss_timestamp_us();
		if (now - last_stats >= 30000000) {
			RSS_INFO("audio frames: %llu", (unsigned long long)frame_count);
			last_stats = now;
		}
	}

	RSS_INFO("rad shutting down");

cleanup:
	if (ao_thread_started)
		pthread_join(ao_tid, NULL);
	if (ctrl)
		rss_ctrl_destroy(ctrl);
	free(encode_buf);
	free(aac_pcm_buf);
#ifdef RAPTOR_AAC
	if (aac_handle)
		faacEncClose(aac_handle);
#endif
#ifdef RAPTOR_OPUS
	if (opus_enc)
		opus_encoder_destroy(opus_enc);
#endif
	if (ring)
		rss_ring_destroy(ring);
	if (hal_ctx) {
		if (ao_enabled)
			RSS_HAL_CALL(ops, ao_deinit, hal_ctx);
		RSS_HAL_CALL(ops, audio_deinit, hal_ctx);
		rss_hal_destroy(hal_ctx);
	}
	rss_config_free(dctx.cfg);
	rss_daemon_cleanup("rad");

	return 0;
}

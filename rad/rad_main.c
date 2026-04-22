/*
 * rad_main.c -- Raptor Audio Daemon
 *
 * Captures PCM audio from the ISP's audio input, encodes via pluggable
 * codec, and publishes to SHM ring buffer "audio".
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

#include <pthread.h>
#include <sys/select.h>

#include <raptor_hal.h>
#include <rss_ipc.h>
#include <rss_common.h>

#include "rad.h"

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
	_Atomic int *ao_flush;
#ifdef RAPTOR_AUDIO_EFFECTS
	bool ns_enabled;
	bool hpf_enabled;
	bool agc_enabled;
#endif
	bool muted;
	bool ao_muted;
	/* Pipeline state — updated by ctrl handler on restart */
	rss_ring_t **ring;
	const rad_codec_ops_t **codec_ops;
	rad_codec_ctx_t *codec_ctx;
	uint8_t **encode_buf;
	int *encode_buf_size;
} rad_ctrl_ctx_t;

static int rad_fmt_result(char *buf, int bufsz, int ret)
{
	if (ret == 0)
		return rss_ctrl_resp_ok(buf, bufsz);
	if (ret == RSS_ERR_NOTSUP)
		return rss_ctrl_resp_error(buf, bufsz, "not supported on this SoC");
	char reason[64];
	snprintf(reason, sizeof(reason), "failed (%d)", ret);
	return rss_ctrl_resp_error(buf, bufsz, reason);
}

static int rad_ctrl_handler(const char *cmd_json, char *resp_buf, int resp_buf_size, void *userdata)
{
	rad_ctrl_ctx_t *ctx = userdata;
	int val;

	int rc = rss_ctrl_handle_common(cmd_json, resp_buf, resp_buf_size, ctx->cfg,
					ctx->config_path);
	if (rc >= 0)
		return rc;

	char cmd[64];
	if (rss_json_get_str(cmd_json, "cmd", cmd, sizeof(cmd)) != 0)
		return rss_ctrl_resp_error(resp_buf, resp_buf_size, "missing cmd");

	if (strcmp(cmd, "set-volume") == 0) {
		if (rss_json_get_int(cmd_json, "value", &val) == 0) {
			int ret = RSS_HAL_CALL(ctx->ops, audio_set_volume, ctx->hal_ctx,
					       ctx->ai_dev, 0, val);
			if (ret == 0) {
				ctx->volume = val;
				rss_config_set_int(ctx->cfg, "audio", "volume", val);
			}
			return rad_fmt_result(resp_buf, resp_buf_size, ret);
		}
		return rss_ctrl_resp_error(resp_buf, resp_buf_size, "need value");
	}

	if (strcmp(cmd, "set-gain") == 0) {
		if (rss_json_get_int(cmd_json, "value", &val) == 0) {
			int ret = RSS_HAL_CALL(ctx->ops, audio_set_gain, ctx->hal_ctx, ctx->ai_dev,
					       0, val);
			if (ret == 0) {
				ctx->gain = val;
				rss_config_set_int(ctx->cfg, "audio", "gain", val);
			}
			return rad_fmt_result(resp_buf, resp_buf_size, ret);
		}
		return rss_ctrl_resp_error(resp_buf, resp_buf_size, "need value");
	}

	if (strcmp(cmd, "set-alc-gain") == 0) {
		if (rss_json_get_int(cmd_json, "value", &val) == 0) {
			int ret = RSS_HAL_CALL(ctx->ops, audio_set_alc_gain, ctx->hal_ctx,
					       ctx->ai_dev, 0, val);
			if (ret == 0) {
				rss_config_set_int(ctx->cfg, "audio", "alc_gain", val);
				return rss_ctrl_resp_ok(resp_buf, resp_buf_size);
			}
			return rss_ctrl_resp_error(resp_buf, resp_buf_size,
						   ret == RSS_ERR_NOTSUP
							   ? "not supported on this platform"
							   : "failed");
		}
		return rss_ctrl_resp_error(resp_buf, resp_buf_size, "need value 0-7");
	}

	if (strcmp(cmd, "mute") == 0) {
		int ret = RSS_HAL_CALL(ctx->ops, audio_set_mute, ctx->hal_ctx, ctx->ai_dev, 0, 1);
		if (ret == 0)
			ctx->muted = true;
		return rad_fmt_result(resp_buf, resp_buf_size, ret);
	}

	if (strcmp(cmd, "unmute") == 0) {
		int ret = RSS_HAL_CALL(ctx->ops, audio_set_mute, ctx->hal_ctx, ctx->ai_dev, 0, 0);
		if (ret == 0)
			ctx->muted = false;
		return rad_fmt_result(resp_buf, resp_buf_size, ret);
	}

#ifdef RAPTOR_AUDIO_EFFECTS
	if (strcmp(cmd, "set-ns") == 0) {
		int level = RSS_NS_MODERATE;
		rss_json_get_int(cmd_json, "level", &level);
		if (rss_json_get_int(cmd_json, "value", &val) == 0) {
			int ret = RSS_OK;
			if (val && !ctx->ns_enabled) {
				if (level < RSS_NS_LOW || level > RSS_NS_VERYHIGH)
					level = RSS_NS_MODERATE;
				ret = RSS_HAL_CALL(ctx->ops, audio_enable_ns, ctx->hal_ctx,
						   (rss_ns_level_t)level);
			} else if (!val && ctx->ns_enabled) {
				ret = RSS_HAL_CALL(ctx->ops, audio_disable_ns, ctx->hal_ctx);
			}
			if (ret == RSS_OK)
				ctx->ns_enabled = !!val;
			if (ret == RSS_ERR_NOTSUP)
				return rss_ctrl_resp_error(resp_buf, resp_buf_size,
							   "not supported on this SoC");
			cJSON *r = cJSON_CreateObject();
			cJSON_AddStringToObject(r, "status", ret == RSS_OK ? "ok" : "error");
			cJSON_AddBoolToObject(r, "ns", ctx->ns_enabled);
			return rss_ctrl_resp_json(resp_buf, resp_buf_size, r);
		}
		return rss_ctrl_resp_error(resp_buf, resp_buf_size, "need value (0/1)");
	}

	if (strcmp(cmd, "set-hpf") == 0) {
		if (rss_json_get_int(cmd_json, "value", &val) == 0) {
			int ret = RSS_OK;
			if (val && !ctx->hpf_enabled)
				ret = RSS_HAL_CALL(ctx->ops, audio_enable_hpf, ctx->hal_ctx);
			else if (!val && ctx->hpf_enabled)
				ret = RSS_HAL_CALL(ctx->ops, audio_disable_hpf, ctx->hal_ctx);
			if (ret == RSS_OK)
				ctx->hpf_enabled = !!val;
			if (ret == RSS_ERR_NOTSUP)
				return rss_ctrl_resp_error(resp_buf, resp_buf_size,
							   "not supported on this SoC");
			cJSON *r = cJSON_CreateObject();
			cJSON_AddStringToObject(r, "status", ret == RSS_OK ? "ok" : "error");
			cJSON_AddBoolToObject(r, "hpf", ctx->hpf_enabled);
			return rss_ctrl_resp_json(resp_buf, resp_buf_size, r);
		}
		return rss_ctrl_resp_error(resp_buf, resp_buf_size, "need value (0/1)");
	}

	if (strcmp(cmd, "set-agc") == 0) {
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
				return rss_ctrl_resp_error(resp_buf, resp_buf_size,
							   "not supported on this SoC");
			cJSON *r = cJSON_CreateObject();
			cJSON_AddStringToObject(r, "status", ret == RSS_OK ? "ok" : "error");
			cJSON_AddBoolToObject(r, "agc", ctx->agc_enabled);
			return rss_ctrl_resp_json(resp_buf, resp_buf_size, r);
		}
		return rss_ctrl_resp_error(resp_buf, resp_buf_size, "need value (0/1)");
	}
#endif /* RAPTOR_AUDIO_EFFECTS */

	if (strcmp(cmd, "ao-set-volume") == 0) {
		if (rss_json_get_int(cmd_json, "value", &val) == 0 && ctx->ao_enabled) {
			RSS_HAL_CALL(ctx->ops, ao_set_volume, ctx->hal_ctx, val);
			ctx->ao_volume = val;
			return rss_ctrl_resp_ok(resp_buf, resp_buf_size);
		}
		return rss_ctrl_resp_error(resp_buf, resp_buf_size,
					   ctx->ao_enabled ? "need value" : "ao disabled");
	}

	if (strcmp(cmd, "ao-set-gain") == 0) {
		if (rss_json_get_int(cmd_json, "value", &val) == 0 && ctx->ao_enabled) {
			RSS_HAL_CALL(ctx->ops, ao_set_gain, ctx->hal_ctx, val);
			ctx->ao_gain = val;
			return rss_ctrl_resp_ok(resp_buf, resp_buf_size);
		}
		return rss_ctrl_resp_error(resp_buf, resp_buf_size,
					   ctx->ao_enabled ? "need value" : "ao disabled");
	}

	if (strcmp(cmd, "ao-mute") == 0) {
		if (!ctx->ao_enabled)
			return rss_ctrl_resp_error(resp_buf, resp_buf_size, "ao disabled");
		int ret = RSS_HAL_CALL(ctx->ops, ao_soft_mute, ctx->hal_ctx, 0, 0);
		if (ret == 0)
			ctx->ao_muted = true;
		return rad_fmt_result(resp_buf, resp_buf_size, ret);
	}

	if (strcmp(cmd, "ao-unmute") == 0) {
		if (!ctx->ao_enabled)
			return rss_ctrl_resp_error(resp_buf, resp_buf_size, "ao disabled");
		int ret = RSS_HAL_CALL(ctx->ops, ao_soft_unmute, ctx->hal_ctx, 0, 0);
		if (ret == 0)
			ctx->ao_muted = false;
		return rad_fmt_result(resp_buf, resp_buf_size, ret);
	}

	if (strcmp(cmd, "ao-flush") == 0) {
		if (ctx->ao_flush)
			atomic_store(ctx->ao_flush, 1);
		return rss_ctrl_resp_ok(resp_buf, resp_buf_size);
	}

	/* ── Audio pipeline restart (codec/sample-rate change) ── */

	if (strcmp(cmd, "set-codec") == 0 || strcmp(cmd, "audio-restart") == 0) {
		char new_codec_str[16] = "";
		rss_json_get_str(cmd_json, "value", new_codec_str, sizeof(new_codec_str));

		/* If no codec specified, restart with current codec */
		const char *target_codec = new_codec_str[0] ? new_codec_str : ctx->codec_str;

		const rad_codec_ops_t *new_ops = rad_codec_find(target_codec);
		if (!new_ops) {
			return rss_ctrl_resp_error(resp_buf, resp_buf_size, "unknown codec");
		}

		int new_codec_id = new_ops->codec_id;
		int new_sample_rate = ctx->sample_rate;

		/* G.711 forces 8kHz; other codecs use configured rate */
		if (new_codec_id == RAD_CODEC_PCMU || new_codec_id == RAD_CODEC_PCMA)
			new_sample_rate = 8000;
		else
			new_sample_rate =
				rss_config_get_int(ctx->cfg, "audio", "sample_rate", 16000);

		/* Save old state for rollback */
		const rad_codec_ops_t *old_ops = *ctx->codec_ops;
		const char *old_codec_str = ctx->codec_str;
		int old_codec_id = ctx->codec_id;
		int old_sample_rate = ctx->sample_rate;

		RSS_INFO("audio-restart: %s %dHz → %s %dHz", ctx->codec_str, ctx->sample_rate,
			 target_codec, new_sample_rate);

		/* 1. Tear down codec */
		if (*ctx->codec_ops && (*ctx->codec_ops)->deinit)
			(*ctx->codec_ops)->deinit(ctx->codec_ctx);
		memset(ctx->codec_ctx, 0, sizeof(*ctx->codec_ctx));

		/* 2. Destroy ring (consumers will detect and reconnect) */
		if (*ctx->ring) {
			rss_ring_destroy(*ctx->ring);
			*ctx->ring = NULL;
		}

		/* 3. Reinit HAL audio (sample rate may have changed) */
		RSS_HAL_CALL(ctx->ops, audio_deinit, ctx->hal_ctx);

		rss_audio_config_t audio_cfg = {
			.sample_rate = new_sample_rate,
			.samples_per_frame = new_sample_rate / 50,
			.chn_count = 1,
			.frame_depth = 20,
			.ai_vol = ctx->volume,
			.ai_gain = ctx->gain,
		};
		int ret = RSS_HAL_CALL(ctx->ops, audio_init, ctx->hal_ctx, &audio_cfg);
		if (ret != RSS_OK) {
			RSS_ERROR("audio-restart: audio_init failed: %d", ret);
			return rss_ctrl_resp_error(resp_buf, resp_buf_size, "audio_init failed");
		}

		RSS_HAL_CALL(ctx->ops, audio_set_volume, ctx->hal_ctx, ctx->ai_dev, 0, ctx->volume);
		RSS_HAL_CALL(ctx->ops, audio_set_gain, ctx->hal_ctx, ctx->ai_dev, 0, ctx->gain);

		/* 4. Re-apply audio effects */
#ifdef RAPTOR_AUDIO_EFFECTS
		if (ctx->ns_enabled) {
			int ns_level =
				rss_config_get_int(ctx->cfg, "audio", "ns_level", RSS_NS_MODERATE);
			RSS_HAL_CALL(ctx->ops, audio_enable_ns, ctx->hal_ctx,
				     (rss_ns_level_t)ns_level);
		}
		if (ctx->hpf_enabled)
			RSS_HAL_CALL(ctx->ops, audio_enable_hpf, ctx->hal_ctx);
		if (ctx->agc_enabled) {
			rss_agc_config_t agc_cfg = {
				.target_level_dbfs = rss_config_get_int(ctx->cfg, "audio",
									"agc_target_dbfs", 10),
				.compression_gain_db = rss_config_get_int(ctx->cfg, "audio",
									  "agc_compression_db", 0),
			};
			RSS_HAL_CALL(ctx->ops, audio_enable_agc, ctx->hal_ctx, &agc_cfg);
		}
#endif

		/* 5. Init new codec — restore old on failure */
		bool codec_switched = true;
		ctx->codec_ctx->codec_id = new_codec_id;
		if (new_ops->init(ctx->codec_ctx, ctx->cfg, new_sample_rate) != 0) {
			RSS_ERROR("audio-restart: codec %s init failed, restoring %s", target_codec,
				  old_codec_str);
			memset(ctx->codec_ctx, 0, sizeof(*ctx->codec_ctx));
			ctx->codec_ctx->codec_id = old_codec_id;
			if (old_ops->init(ctx->codec_ctx, ctx->cfg, old_sample_rate) != 0)
				RSS_FATAL("audio-restart: old codec %s restore also failed",
					  old_codec_str);
			new_ops = old_ops;
			new_codec_id = old_codec_id;
			new_sample_rate = old_sample_rate;
			codec_switched = false;
		}

		/* 6. Create new ring — restore old codec on failure */
		int ring_data_size = (new_codec_id == RAD_CODEC_L16) ? 256 * 1024 : 128 * 1024;
		*ctx->ring = rss_ring_create("audio", 32, ring_data_size);
		if (!*ctx->ring) {
			RSS_ERROR("audio-restart: ring create failed, retrying");
			*ctx->ring = rss_ring_create("audio", 32, 128 * 1024);
			if (!*ctx->ring)
				RSS_FATAL("audio-restart: ring create failed on retry");
		}
		rss_ring_set_stream_info(*ctx->ring, 0x10, new_codec_id, 0, 0, new_sample_rate, 1,
					 0, 0);
		ctx->codec_ctx->ring = *ctx->ring;

		/* 7. Resize encode buffer if needed */
		int new_buf_size = ctx->codec_ctx->encode_buf_size;
		if (new_buf_size < new_sample_rate / 50 * 2)
			new_buf_size = new_sample_rate / 50 * 2;
		if (new_buf_size > *ctx->encode_buf_size) {
			uint8_t *new_buf = malloc(new_buf_size);
			if (!new_buf) {
				RSS_ERROR("audio-restart: encode buf alloc failed");
				return rss_ctrl_resp_error(resp_buf, resp_buf_size, "alloc failed");
			}
			free(*ctx->encode_buf);
			*ctx->encode_buf = new_buf;
		}
		*ctx->encode_buf_size = new_buf_size;

		/* 8. Update state — persist config only after successful switch */
		*ctx->codec_ops = new_ops;
		ctx->codec_id = new_codec_id;
		ctx->codec_str = new_ops->name;
		ctx->sample_rate = new_sample_rate;
		if (codec_switched)
			rss_config_set_str(ctx->cfg, "audio", "codec", target_codec);

		RSS_INFO("audio-restart: %s %dHz ready", new_ops->name, new_sample_rate);
		if (!codec_switched)
			return rss_ctrl_resp_error(resp_buf, resp_buf_size,
						   "codec init failed, restored old codec");
		return rss_ctrl_resp_ok(resp_buf, resp_buf_size);
	}

	if (strcmp(cmd, "config-show") == 0) {
		cJSON *r = cJSON_CreateObject();
		cJSON_AddStringToObject(r, "status", "ok");
		cJSON *config = cJSON_AddObjectToObject(r, "config");
		cJSON_AddStringToObject(config, "codec", ctx->codec_str);
		cJSON_AddNumberToObject(config, "sample_rate", ctx->sample_rate);
		cJSON_AddNumberToObject(config, "volume", ctx->volume);
		cJSON_AddNumberToObject(config, "gain", ctx->gain);
		cJSON_AddNumberToObject(config, "device", ctx->ai_dev);
		cJSON_AddStringToObject(config, "config_path", ctx->config_path);
		return rss_ctrl_resp_json(resp_buf, resp_buf_size, r);
	}

	if (strcmp(cmd, "status") == 0) {
		cJSON *r = cJSON_CreateObject();
		cJSON_AddStringToObject(r, "status", "ok");
		cJSON_AddStringToObject(r, "codec", ctx->codec_str);
		cJSON_AddNumberToObject(r, "sample_rate", ctx->sample_rate);
		cJSON_AddNumberToObject(r, "volume", ctx->volume);
		cJSON_AddNumberToObject(r, "gain", ctx->gain);
		cJSON_AddBoolToObject(r, "muted", ctx->muted);
		cJSON_AddBoolToObject(r, "ao_enabled", ctx->ao_enabled);
		if (ctx->ao_enabled) {
			cJSON_AddNumberToObject(r, "ao_volume", ctx->ao_volume);
			cJSON_AddNumberToObject(r, "ao_gain", ctx->ao_gain);
			cJSON_AddBoolToObject(r, "ao_muted", ctx->ao_muted);
		}
#ifdef RAPTOR_AUDIO_EFFECTS
		cJSON_AddBoolToObject(r, "ns", ctx->ns_enabled);
		cJSON_AddBoolToObject(r, "hpf", ctx->hpf_enabled);
		cJSON_AddBoolToObject(r, "agc", ctx->agc_enabled);
#endif
		return rss_ctrl_resp_json(resp_buf, resp_buf_size, r);
	}

	return rss_ctrl_resp_error(resp_buf, resp_buf_size, "unknown command");
}

/* ── AO playback thread: reads from speaker ring, sends to hardware ── */

typedef struct {
	const rss_hal_ops_t *ops;
	rss_hal_ctx_t *hal_ctx;
	volatile sig_atomic_t *running;
	_Atomic int flush;
} ao_thread_ctx_t;

static void *ao_playback_thread(void *arg)
{
	ao_thread_ctx_t *ctx = arg;
	rss_ring_t *ring = NULL;
	uint8_t *buf = NULL;

	RSS_DEBUG("ao playback thread started");

	while (*ctx->running) {
		/* ── Phase 1: wait for speaker ring ── */
		while (*ctx->running && !ring) {
			if (atomic_exchange(&ctx->flush, 0)) {
				RSS_HAL_CALL(ctx->ops, ao_clear_buf, ctx->hal_ctx);
				RSS_DEBUG("ao: flushed hardware buffer");
			}
			ring = rss_ring_open("speaker");
			if (ring) {
				rss_ring_check_version(ring, "speaker");
			} else {
				usleep(50000);
			}
		}
		if (!ring)
			break;

		/* Clear hardware buffer — prevents stale audio from previous
		 * playback bleeding into the new file */
		RSS_HAL_CALL(ctx->ops, ao_clear_buf, ctx->hal_ctx);
		rss_ring_acquire(ring);

		const rss_ring_header_t *hdr = rss_ring_get_header(ring);
		buf = malloc(hdr->data_size);
		if (!buf) {
			RSS_FATAL("ao thread: malloc failed");
			rss_ring_release(ring);
			rss_ring_close(ring);
			ring = NULL;
			break;
		}

		uint64_t read_seq = 0;
		uint64_t last_write_seq = atomic_load(&hdr->write_seq);
		int idle_count = 0;
		RSS_DEBUG("speaker ring connected");

		/* ── Phase 2: read and play frames ── */
		while (*ctx->running) {
			if (atomic_exchange(&ctx->flush, 0)) {
				RSS_HAL_CALL(ctx->ops, ao_clear_buf, ctx->hal_ctx);
				RSS_DEBUG("ao: flush requested, dropping ring");
				break;
			}

			int ret = rss_ring_wait(ring, 50);
			if (ret != 0) {
				uint64_t ws = atomic_load(&hdr->write_seq);
				if (ws == last_write_seq)
					idle_count++;
				else
					idle_count = 0;
				last_write_seq = ws;

				/* ~500ms idle (10 × 50ms) — close and wait for fresh ring */
				if (idle_count >= 10) {
					RSS_DEBUG("speaker idle, closing ring");
					break;
				}
				continue;
			}
			idle_count = 0;

			uint32_t length = 0;
			rss_ring_slot_t meta;
			ret = rss_ring_read(ring, &read_seq, buf, hdr->data_size, &length, &meta);
			if (ret != 0) {
				RSS_DEBUG("ao ring_read: %d seq=%llu", ret,
					  (unsigned long long)read_seq);
				continue;
			}

			RSS_HAL_CALL(ctx->ops, ao_send_frame, ctx->hal_ctx, (const int16_t *)buf,
				     length, true);
		}

		/* Cleanup ring before looping back to Phase 1 */
		free(buf);
		buf = NULL;
		rss_ring_release(ring);
		rss_ring_close(ring);
		ring = NULL;
	}

	free(buf);
	if (ring) {
		rss_ring_release(ring);
		rss_ring_close(ring);
	}
	RSS_DEBUG("ao playback thread exiting");
	return NULL;
}

/* Bridge HAL logging into the daemon's syslog-aware logger */
static const rss_log_level_t hal_level_map[] = {
	[0] = RSS_LOG_FATAL, [1] = RSS_LOG_ERROR, [2] = RSS_LOG_WARN,
	[3] = RSS_LOG_INFO,  [4] = RSS_LOG_DEBUG,
};

static void hal_log_bridge(int level, const char *file, int line, const char *fmt, ...)
{
	rss_log_level_t lvl = (level >= 0 && level <= 4) ? hal_level_map[level] : RSS_LOG_DEBUG;
	va_list ap;
	va_start(ap, fmt);
	rss_vlog(lvl, file, line, fmt, ap);
	va_end(ap);
}

int main(int argc, char **argv)
{
	rss_daemon_ctx_t dctx;
	int ret = rss_daemon_init(&dctx, "rad", argc, argv,
				  ""
#ifdef RAPTOR_AAC
				  " aac"
#endif
#ifdef RAPTOR_OPUS
				  " opus"
#endif
#ifdef RAPTOR_AUDIO_EFFECTS
				  " effects"
#endif
	);
	if (ret != 0)
		return ret < 0 ? 1 : 0;
	rss_hal_set_log_func(hal_log_bridge);

	if (!rss_config_get_bool(dctx.cfg, "audio", "enabled", true)) {
		RSS_INFO("audio disabled in config");
		rss_config_free(dctx.cfg);
		rss_daemon_cleanup("rad");
		return 0;
	}

	rss_ctrl_t *ctrl = NULL;
	rss_ring_t *ring = NULL;
	uint8_t *encode_buf = NULL;
	const rad_codec_ops_t *codec_ops = NULL;
	rad_codec_ctx_t codec_ctx = {0};
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

	/* Find codec plugin */
	codec_ops = rad_codec_find(codec_str);
	if (!codec_ops) {
		RSS_FATAL("unknown audio codec: %s", codec_str);
		goto cleanup;
	}
	int codec_id = codec_ops->codec_id;

	/* G.711 is 8kHz only */
	if (codec_id == RAD_CODEC_PCMU || codec_id == RAD_CODEC_PCMA)
		sample_rate = 8000;

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

	/* ── Codec init ── */
	codec_ctx.codec_id = codec_id;
	if (codec_ops->init(&codec_ctx, dctx.cfg, sample_rate) != 0) {
		RSS_FATAL("codec %s init failed", codec_str);
		goto cleanup;
	}

	/* ── Audio effects (libaudioProcess.so) ── */
#ifdef RAPTOR_AUDIO_EFFECTS
	bool ns_enabled = rss_config_get_bool(dctx.cfg, "audio", "ns_enabled", false);
	if (ns_enabled) {
		int ns_level = rss_config_get_int(dctx.cfg, "audio", "ns_level", RSS_NS_MODERATE);
		if (ns_level < RSS_NS_LOW || ns_level > RSS_NS_VERYHIGH)
			ns_level = RSS_NS_MODERATE;
		ret = RSS_HAL_CALL(ops, audio_enable_ns, hal_ctx, (rss_ns_level_t)ns_level);
		if (ret == RSS_OK)
			RSS_DEBUG("noise suppression: level %d", ns_level);
		else
			RSS_WARN("noise suppression failed: %d", ret);
	}

	bool hpf_enabled = rss_config_get_bool(dctx.cfg, "audio", "hpf_enabled", false);
	if (hpf_enabled) {
		ret = RSS_HAL_CALL(ops, audio_enable_hpf, hal_ctx);
		if (ret == RSS_OK)
			RSS_DEBUG("high-pass filter enabled");
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
			RSS_DEBUG("agc: target=%d dBfs, compression=%d dB",
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
				RSS_DEBUG("audio output: %d Hz vol=%d gain=%d", sample_rate, ao_vol,
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

	/* Encode buffer — sized by codec plugin */
	int encode_buf_size = codec_ctx.encode_buf_size;
	if (encode_buf_size < (int)audio_cfg.samples_per_frame * 2)
		encode_buf_size = audio_cfg.samples_per_frame * 2;
	encode_buf = malloc(encode_buf_size);
	if (!encode_buf) {
		RSS_FATAL("failed to allocate encode buffer");
		goto cleanup;
	}

	/* Give codec access to ring for direct publish (AAC accumulation) */
	codec_ctx.ring = ring;

	/* Control socket */
	rss_mkdir_p(RSS_RUN_DIR);
	ctrl = rss_ctrl_listen(RSS_RUN_DIR "/rad.sock");
	if (!ctrl)
		RSS_WARN("control socket failed (non-fatal)");

	RSS_DEBUG("audio loop: %d samples/frame (%dms), %s", audio_cfg.samples_per_frame, 1000 / 50,
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
		.ao_flush = ao_enabled ? &ao_ctx.flush : NULL,
#ifdef RAPTOR_AUDIO_EFFECTS
		.ns_enabled = ns_enabled,
		.hpf_enabled = hpf_enabled,
		.agc_enabled = agc_enabled,
#endif
		/* Pipeline state pointers (for audio-restart) */
		.ring = &ring,
		.codec_ops = &codec_ops,
		.codec_ctx = &codec_ctx,
		.encode_buf = &encode_buf,
		.encode_buf_size = &encode_buf_size,
	};

	uint64_t frame_count = 0;
	int64_t last_stats = rss_timestamp_us();
	int64_t synth_audio_ts = rss_timestamp_us();

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
		int max_samples = ctrl_ctx.sample_rate / 50; /* 20ms */
		if (samples > max_samples)
			samples = max_samples;

		/* Always use synthetic timestamps from IMP_System_GetTimeStamp.
		 * SDK audio timestamps use a different clock than the encoder
		 * on some SoCs (T31), causing A-V sync drift. Synthetic
		 * timestamps share the encoder's clock source. */
		int64_t ts = synth_audio_ts;
		synth_audio_ts += (int64_t)samples * 1000000 / ctrl_ctx.sample_rate;

		const int16_t *pcm = frame.data;
		int out_len;

		out_len = codec_ops->encode(&codec_ctx, pcm, samples, encode_buf, encode_buf_size,
					    ts);

		if (out_len > 0)
			rss_ring_publish(ring, encode_buf, out_len, ts, ctrl_ctx.codec_id, 0);

		RSS_HAL_CALL(ops, audio_release_frame, hal_ctx, ai_dev, 0, &frame);

		frame_count++;

		int64_t now = rss_timestamp_us();
		if (now - last_stats >= 30000000) {
			RSS_DEBUG("audio frames: %llu", (unsigned long long)frame_count);
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
	if (codec_ops && codec_ops->deinit)
		codec_ops->deinit(&codec_ctx);
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

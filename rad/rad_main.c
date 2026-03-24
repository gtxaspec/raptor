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
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <arpa/inet.h>

#include <sys/select.h>

#include <raptor_hal.h>
#include <rss_ipc.h>
#include <rss_common.h>

/* Audio codec IDs stored in ring stream_info */
#define RAD_CODEC_PCMU 0
#define RAD_CODEC_PCMA 8
#define RAD_CODEC_L16  11

/* ── G.711 mu-law encoding ── */

static uint8_t pcm16_to_ulaw(int16_t pcm)
{
	int sign, exponent, mantissa;
	const int BIAS = 0x84;

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
#ifdef RAPTOR_AUDIO_EFFECTS
	bool ns_enabled;
	bool hpf_enabled;
	bool agc_enabled;
#endif
} rad_ctrl_ctx_t;

static int json_get_int(const char *json, const char *key, int *out)
{
	char pattern[64];
	snprintf(pattern, sizeof(pattern), "\"%s\":", key);
	const char *p = strstr(json, pattern);
	if (!p)
		return -1;
	p += strlen(pattern);
	while (*p == ' ')
		p++;
	*out = atoi(p);
	return 0;
}

static int json_get_str(const char *json, const char *key, char *buf, int bufsz)
{
	char pattern[64];
	snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
	const char *p = strstr(json, pattern);
	if (!p)
		return -1;
	p += strlen(pattern);
	const char *end = strchr(p, '"');
	if (!end)
		return -1;
	int len = (int)(end - p);
	if (len >= bufsz)
		len = bufsz - 1;
	memcpy(buf, p, len);
	buf[len] = '\0';
	return 0;
}

#define CTRL_RESP(buf) return (int)strlen(buf)

static int rad_ctrl_handler(const char *cmd_json, char *resp_buf, int resp_buf_size, void *userdata)
{
	rad_ctrl_ctx_t *ctx = userdata;
	int val;

	if (strstr(cmd_json, "\"config-get\"")) {
		char section[64], key[64];
		if (json_get_str(cmd_json, "section", section, sizeof(section)) == 0 &&
		    json_get_str(cmd_json, "key", key, sizeof(key)) == 0) {
			const char *v = rss_config_get_str(ctx->cfg, section, key, NULL);
			if (v)
				snprintf(resp_buf, resp_buf_size, "%s", v);
			else
				resp_buf[0] = '\0';
		} else {
			resp_buf[0] = '\0';
		}
		CTRL_RESP(resp_buf);
	}

	if (strstr(cmd_json, "\"set-volume\"")) {
		if (json_get_int(cmd_json, "value", &val) == 0) {
			RSS_HAL_CALL(ctx->ops, audio_set_volume, ctx->hal_ctx, ctx->ai_dev, 0, val);
			ctx->volume = val;
			rss_config_set_int(ctx->cfg, "audio", "volume", val);
			snprintf(resp_buf, resp_buf_size, "{\"status\":\"ok\"}");
		} else {
			snprintf(resp_buf, resp_buf_size,
				 "{\"status\":\"error\",\"reason\":\"need value\"}");
		}
		CTRL_RESP(resp_buf);
	}

	if (strstr(cmd_json, "\"set-gain\"")) {
		if (json_get_int(cmd_json, "value", &val) == 0) {
			RSS_HAL_CALL(ctx->ops, audio_set_gain, ctx->hal_ctx, ctx->ai_dev, 0, val);
			ctx->gain = val;
			rss_config_set_int(ctx->cfg, "audio", "gain", val);
			snprintf(resp_buf, resp_buf_size, "{\"status\":\"ok\"}");
		} else {
			snprintf(resp_buf, resp_buf_size,
				 "{\"status\":\"error\",\"reason\":\"need value\"}");
		}
		CTRL_RESP(resp_buf);
	}

#ifdef RAPTOR_AUDIO_EFFECTS
	if (strstr(cmd_json, "\"set-ns\"")) {
		char level[16] = "";
		json_get_str(cmd_json, "level", level, sizeof(level));
		if (json_get_int(cmd_json, "value", &val) == 0) {
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
		if (json_get_int(cmd_json, "value", &val) == 0) {
			int ret = RSS_OK;
			if (val && !ctx->hpf_enabled)
				ret = RSS_HAL_CALL(ctx->ops, audio_enable_hpf, ctx->hal_ctx);
			else if (!val && ctx->hpf_enabled)
				ret = RSS_HAL_CALL(ctx->ops, audio_disable_hpf, ctx->hal_ctx);
			if (ret == RSS_OK)
				ctx->hpf_enabled = !!val;
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
		if (json_get_int(cmd_json, "value", &val) == 0) {
			int ret = RSS_OK;
			if (val) {
				int target = 10, compression = 0;
				json_get_int(cmd_json, "target", &target);
				json_get_int(cmd_json, "compression", &compression);
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

	if (strstr(cmd_json, "\"config-save\"")) {
		int ret = rss_config_save(ctx->cfg, ctx->config_path);
		snprintf(resp_buf, resp_buf_size, "{\"status\":\"%s\"}", ret == 0 ? "ok" : "error");
		if (ret == 0)
			RSS_INFO("running config saved to %s", ctx->config_path);
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
		snprintf(resp_buf, resp_buf_size,
			 "{\"status\":\"ok\",\"codec\":\"%s\","
			 "\"sample_rate\":%d,\"volume\":%d,\"gain\":%d"
#ifdef RAPTOR_AUDIO_EFFECTS
			 ",\"ns\":%s,\"hpf\":%s,\"agc\":%s"
#endif
			 "}",
			 ctx->codec_str, ctx->sample_rate, ctx->volume, ctx->gain
#ifdef RAPTOR_AUDIO_EFFECTS
			 ,
			 ctx->ns_enabled ? "true" : "false", ctx->hpf_enabled ? "true" : "false",
			 ctx->agc_enabled ? "true" : "false"
#endif
		);
		CTRL_RESP(resp_buf);
	}

	snprintf(resp_buf, resp_buf_size, "{\"status\":\"error\",\"reason\":\"unknown command\"}");
	CTRL_RESP(resp_buf);
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"  -c <file>   Config file (default: /etc/raptor.conf)\n"
		"  -f          Run in foreground\n"
		"  -d          Debug logging\n"
		"  -h          Show this help\n",
		prog);
}

int main(int argc, char **argv)
{
	const char *config_path = "/etc/raptor.conf";
	bool foreground = false;
	bool debug = false;
	int opt;

	while ((opt = getopt(argc, argv, "c:fdh")) != -1) {
		switch (opt) {
		case 'c':
			config_path = optarg;
			break;
		case 'f':
			foreground = true;
			break;
		case 'd':
			debug = true;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	rss_log_init("rad", debug ? RSS_LOG_DEBUG : RSS_LOG_INFO,
		     foreground ? RSS_LOG_TARGET_STDERR : RSS_LOG_TARGET_SYSLOG, NULL);

	rss_config_t *cfg = rss_config_load(config_path);
	if (!cfg) {
		RSS_FATAL("failed to load config: %s", config_path);
		return 1;
	}

	if (!rss_config_get_bool(cfg, "audio", "enabled", true)) {
		RSS_INFO("audio disabled in config");
		rss_config_free(cfg);
		return 0;
	}

	if (!foreground) {
		if (rss_daemonize("rad", false) < 0) {
			RSS_FATAL("daemonize failed");
			rss_config_free(cfg);
			return 1;
		}
	}

	volatile sig_atomic_t *running = rss_signal_init();
	RSS_INFO("rad starting");

	rss_ctrl_t *ctrl = NULL;
	rss_ring_t *ring = NULL;
	uint8_t *encode_buf = NULL;

	rss_hal_ctx_t *hal_ctx = rss_hal_create();
	if (!hal_ctx) {
		RSS_FATAL("rss_hal_create failed");
		goto cleanup;
	}
	const rss_hal_ops_t *ops = rss_hal_get_ops(hal_ctx);

	/* Audio config */
	int sample_rate = rss_config_get_int(cfg, "audio", "sample_rate", 16000);
	int volume = rss_config_get_int(cfg, "audio", "volume", 80);
	int gain = rss_config_get_int(cfg, "audio", "gain", 25);
	int ai_dev = rss_config_get_int(cfg, "audio", "device", 1);
	const char *codec_str = rss_config_get_str(cfg, "audio", "codec", "l16");

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
		/* sample_rate is configurable for L16 */
	} else {
		RSS_FATAL("unknown audio codec: %s (use pcmu, pcma, or l16)", codec_str);
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

	int ret = RSS_HAL_CALL(ops, audio_init, hal_ctx, &audio_cfg);
	if (ret != RSS_OK) {
		RSS_FATAL("audio_init failed: %d", ret);
		goto cleanup;
	}

	RSS_HAL_CALL(ops, audio_set_volume, hal_ctx, ai_dev, 0, volume);
	RSS_HAL_CALL(ops, audio_set_gain, hal_ctx, ai_dev, 0, gain);

	RSS_INFO("audio: dev=%d %d Hz %s vol=%d gain=%d", ai_dev, sample_rate, codec_str, volume,
		 gain);

	/* ── Audio effects (libaudioProcess.so) ── */
#ifdef RAPTOR_AUDIO_EFFECTS
	bool ns_enabled = rss_config_get_bool(cfg, "audio", "ns_enabled", false);
	if (ns_enabled) {
		const char *ns_str = rss_config_get_str(cfg, "audio", "ns_level", "moderate");
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

	bool hpf_enabled = rss_config_get_bool(cfg, "audio", "hpf_enabled", false);
	if (hpf_enabled) {
		ret = RSS_HAL_CALL(ops, audio_enable_hpf, hal_ctx);
		if (ret == RSS_OK)
			RSS_INFO("high-pass filter enabled");
		else
			RSS_WARN("high-pass filter failed: %d", ret);
	}

	bool agc_enabled = rss_config_get_bool(cfg, "audio", "agc_enabled", false);
	if (agc_enabled) {
		rss_agc_config_t agc_cfg = {
			.target_level_dbfs =
				rss_config_get_int(cfg, "audio", "agc_target_dbfs", 10),
			.compression_gain_db =
				rss_config_get_int(cfg, "audio", "agc_compression_db", 0),
		};
		ret = RSS_HAL_CALL(ops, audio_enable_agc, hal_ctx, &agc_cfg);
		if (ret == RSS_OK)
			RSS_INFO("agc: target=%d dBfs, compression=%d dB",
				 agc_cfg.target_level_dbfs, agc_cfg.compression_gain_db);
		else
			RSS_WARN("agc failed: %d", ret);
	}
#endif

	/* Ring buffer — L16 frames are larger (2 bytes/sample vs 1) */
	int ring_data_size = (codec_id == RAD_CODEC_L16) ? 256 * 1024 : 128 * 1024;
	ring = rss_ring_create("audio", 32, ring_data_size);
	if (!ring) {
		RSS_FATAL("failed to create audio ring");
		goto cleanup;
	}
	rss_ring_set_stream_info(ring, 0x10, codec_id, 0, 0, sample_rate, 1, 0, 0);

	/* Encode buffer: L16 = 2 bytes/sample, G.711 = 1 byte/sample */
	int encode_buf_size = (codec_id == RAD_CODEC_L16) ? audio_cfg.samples_per_frame * 2
							  : audio_cfg.samples_per_frame;
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

	RSS_INFO("audio loop: %d samples/frame (%dms), %s", audio_cfg.samples_per_frame, 1000 / 50,
		 codec_str);

	rad_ctrl_ctx_t ctrl_ctx = {
		cfg,	    config_path, ops,	      hal_ctx,	ai_dev,
		volume,	    gain,	 sample_rate, codec_id, codec_str,
#ifdef RAPTOR_AUDIO_EFFECTS
		ns_enabled, hpf_enabled, agc_enabled,
#endif
	};

	uint64_t frame_count = 0;
	int64_t last_stats = rss_timestamp_us();

	while (*running) {
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
	if (ctrl)
		rss_ctrl_destroy(ctrl);
	free(encode_buf);
	if (ring)
		rss_ring_destroy(ring);
	if (hal_ctx) {
		RSS_HAL_CALL(ops, audio_deinit, hal_ctx);
		rss_hal_destroy(hal_ctx);
	}
	rss_config_free(cfg);
	if (!foreground)
		rss_daemon_cleanup("rad");

	return 0;
}

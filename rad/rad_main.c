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

#include <raptor_hal.h>
#include <rss_ipc.h>
#include <rss_common.h>

/* Audio codec IDs stored in ring stream_info */
#define RAD_CODEC_PCMU  0
#define RAD_CODEC_PCMA  8
#define RAD_CODEC_L16   11

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
	for (int mask = 0x4000; !(pcm & mask) && exponent > 0;
	     exponent--, mask >>= 1)
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
		for (int mask = 0x4000; !(pcm & mask) && exponent > 1;
		     exponent--, mask >>= 1)
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
	for (int i = 0; i < samples; i++) {
		uint16_t be = htons((uint16_t)pcm[i]);
		out[i * 2]     = (uint8_t)(be >> 8);
		out[i * 2 + 1] = (uint8_t)(be & 0xff);
	}
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
		case 'c': config_path = optarg; break;
		case 'f': foreground = true; break;
		case 'd': debug = true; break;
		case 'h': usage(argv[0]); return 0;
		default:  usage(argv[0]); return 1;
		}
	}

	rss_log_init("rad",
		     debug ? RSS_LOG_DEBUG : RSS_LOG_INFO,
		     foreground ? RSS_LOG_TARGET_STDERR : RSS_LOG_TARGET_SYSLOG,
		     NULL);

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
		sample_rate = 8000;  /* PCMU is 8kHz only */
	} else if (strcasecmp(codec_str, "pcma") == 0) {
		codec_id = RAD_CODEC_PCMA;
		sample_rate = 8000;  /* PCMA is 8kHz only */
	} else if (strcasecmp(codec_str, "l16") == 0) {
		codec_id = RAD_CODEC_L16;
		/* sample_rate is configurable for L16 */
	} else {
		RSS_FATAL("unknown audio codec: %s (use pcmu, pcma, or l16)",
			  codec_str);
		goto cleanup;
	}

	rss_audio_config_t audio_cfg = {
		.sample_rate      = sample_rate,
		.samples_per_frame = sample_rate / 50,  /* 20ms frames */
		.chn_count        = 1,
		.frame_depth      = 20,
		.ai_vol           = volume,
		.ai_gain          = gain,
	};

	int ret = RSS_HAL_CALL(ops, audio_init, hal_ctx, &audio_cfg);
	if (ret != RSS_OK) {
		RSS_FATAL("audio_init failed: %d", ret);
		goto cleanup;
	}

	RSS_HAL_CALL(ops, audio_set_volume, hal_ctx, ai_dev, 0, volume);
	RSS_HAL_CALL(ops, audio_set_gain, hal_ctx, ai_dev, 0, gain);

	RSS_INFO("audio: dev=%d %d Hz %s vol=%d gain=%d",
		 ai_dev, sample_rate, codec_str, volume, gain);

	/* Ring buffer — L16 frames are larger (2 bytes/sample vs 1) */
	int ring_data_size = (codec_id == RAD_CODEC_L16) ? 256 * 1024 : 128 * 1024;
	rss_ring_t *ring = rss_ring_create("audio", 32, ring_data_size);
	if (!ring) {
		RSS_FATAL("failed to create audio ring");
		goto cleanup;
	}
	rss_ring_set_stream_info(ring, 0x10, codec_id, 0, 0, sample_rate, 1);

	/* Encode buffer: L16 = 2 bytes/sample, G.711 = 1 byte/sample */
	int encode_buf_size = (codec_id == RAD_CODEC_L16)
		? audio_cfg.samples_per_frame * 2
		: audio_cfg.samples_per_frame;
	uint8_t *encode_buf = malloc(encode_buf_size);
	if (!encode_buf) {
		RSS_FATAL("failed to allocate encode buffer");
		goto cleanup;
	}

	RSS_INFO("audio loop: %d samples/frame (%dms), %s",
		 audio_cfg.samples_per_frame,
		 1000 / 50, codec_str);

	uint64_t frame_count = 0;
	int64_t last_stats = rss_timestamp_us();

	while (*running) {
		rss_audio_frame_t frame;
		ret = RSS_HAL_CALL(ops, audio_read_frame, hal_ctx,
				   ai_dev, 0, &frame, true);
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
			rss_ring_publish(ring, encode_buf, out_len,
					 frame.timestamp, codec_id, 0);

		if (frame._priv) {
			extern int IMP_AI_ReleaseFrame(int devId, int chnId,
						       void *frame);
			IMP_AI_ReleaseFrame(ai_dev, 0, frame._priv);
			free(frame._priv);
		}

		frame_count++;

		int64_t now = rss_timestamp_us();
		if (now - last_stats >= 30000000) {
			RSS_INFO("audio frames: %llu",
				 (unsigned long long)frame_count);
			last_stats = now;
		}
	}

	RSS_INFO("rad shutting down");

cleanup:
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

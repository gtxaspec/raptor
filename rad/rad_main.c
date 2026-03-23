/*
 * rad_main.c -- Raptor Audio Daemon
 *
 * Captures PCM audio from the ISP's audio input, encodes to G.711
 * mu-law (PCMU), and publishes to SHM ring buffer "audio".
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>

#include <raptor_hal.h>
#include <rss_ipc.h>
#include <rss_common.h>

/* G.711 mu-law encoding table (PCM 16-bit → 8-bit mu-law) */
static uint8_t pcm16_to_ulaw(int16_t pcm)
{
	int sign, exponent, mantissa;
	uint8_t ulawbyte;
	const int BIAS = 0x84;

	/* Get the sign and make positive */
	sign = (pcm >> 8) & 0x80;
	if (sign)
		pcm = -pcm;

	/* Clip to 32635 */
	if (pcm > 32635)
		pcm = 32635;

	pcm += BIAS;

	/* Find the exponent */
	exponent = 7;
	for (int mask = 0x4000; !(pcm & mask) && exponent > 0;
	     exponent--, mask >>= 1)
		;

	/* Get the mantissa */
	mantissa = (pcm >> (exponent + 3)) & 0x0f;

	ulawbyte = ~(sign | (exponent << 4) | mantissa);

	return ulawbyte;
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

	/* Create HAL (separate context from RVD — audio subsystem is independent) */
	rss_hal_ctx_t *hal_ctx = rss_hal_create();
	if (!hal_ctx) {
		RSS_FATAL("rss_hal_create failed");
		goto cleanup;
	}
	const rss_hal_ops_t *ops = rss_hal_get_ops(hal_ctx);

	/* Audio config */
	int sample_rate = rss_config_get_int(cfg, "audio", "sample_rate", 8000);
	int volume = rss_config_get_int(cfg, "audio", "volume", 80);
	int gain = rss_config_get_int(cfg, "audio", "gain", 25);
	int ai_dev = rss_config_get_int(cfg, "audio", "device", 1);

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

	/* Override volume/gain on the configured device
	 * (HAL defaults to device 0, but analog mic is often device 1) */
	RSS_HAL_CALL(ops, audio_set_volume, hal_ctx, ai_dev, 0, volume);
	RSS_HAL_CALL(ops, audio_set_gain, hal_ctx, ai_dev, 0, gain);

	RSS_INFO("audio initialized: dev=%d %d Hz, vol=%d, gain=%d",
		 ai_dev, sample_rate, volume, gain);

	/* Create audio ring buffer */
	rss_ring_t *ring = rss_ring_create("audio", 32, 128 * 1024);
	if (!ring) {
		RSS_FATAL("failed to create audio ring");
		goto cleanup;
	}
	rss_ring_set_stream_info(ring, 0x10, 0 /* pcmu */, 0, 0,
				 sample_rate, 1);

	/* Mu-law encode buffer */
	uint8_t *ulaw_buf = malloc(audio_cfg.samples_per_frame);
	if (!ulaw_buf) {
		RSS_FATAL("failed to allocate ulaw buffer");
		goto cleanup;
	}

	RSS_INFO("audio loop starting (20ms frames, %d samples/frame)",
		 audio_cfg.samples_per_frame);

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

		/* Encode PCM16 → G.711 mu-law */
		int samples = frame.length / 2;  /* 16-bit samples */
		if (samples > (int)audio_cfg.samples_per_frame)
			samples = audio_cfg.samples_per_frame;

		const int16_t *pcm = frame.data;
		for (int i = 0; i < samples; i++)
			ulaw_buf[i] = pcm16_to_ulaw(pcm[i]);

		/* Publish to ring */
		rss_ring_publish(ring, ulaw_buf, samples,
				 frame.timestamp, 0 /* pcmu */, 0);

		/* Release HAL frame — need dev/chn for IMP_AI_ReleaseFrame */
		/* The HAL stores the IMPAudioFrame in _priv */
		if (frame._priv) {
			/* Direct SDK call since the HAL vtable doesn't have
			 * a clean release with frame parameter */
			extern int IMP_AI_ReleaseFrame(int devId, int chnId, void *frame);
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
	free(ulaw_buf);
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

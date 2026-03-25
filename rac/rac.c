/*
 * rac.c -- Raptor Audio Client
 *
 * CLI tool for audio input/output:
 *   rac record <file|->         Record mic to file or stdout (PCM16 LE)
 *   rac play <file|->           Play PCM16 LE to speaker via "speaker" ring
 *   rac status                  Show audio daemon status
 *   rac ao-volume <val>         Set speaker volume
 *   rac ao-gain <val>           Set speaker gain
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <errno.h>

#include <rss_ipc.h>
#include <rss_common.h>

static volatile sig_atomic_t g_running = 1;

static void sighandler(int sig)
{
	(void)sig;
	g_running = 0;
}

/* ── G.711 mu-law decode ── */

static int16_t ulaw_to_pcm16(uint8_t ulaw)
{
	ulaw = ~ulaw;
	int sign = (ulaw & 0x80);
	int exponent = (ulaw >> 4) & 0x07;
	int mantissa = ulaw & 0x0f;
	int magnitude = ((mantissa << 3) + 0x84) << exponent;
	magnitude -= 0x84;
	return (int16_t)(sign ? -magnitude : magnitude);
}

/* ── G.711 A-law decode ── */

static int16_t alaw_to_pcm16(uint8_t alaw)
{
	alaw ^= 0xD5;
	int sign = (alaw & 0x80);
	int exponent = (alaw >> 4) & 0x07;
	int mantissa = alaw & 0x0f;
	int magnitude;
	if (exponent > 0)
		magnitude = ((mantissa << 4) + 0x108) << (exponent - 1);
	else
		magnitude = (mantissa << 4) + 8;
	return (int16_t)(sign ? magnitude : -magnitude);
}

/* ── Record: read from audio ring, write PCM16 LE ── */

static int cmd_record(const char *dest, int max_seconds)
{
	FILE *out = NULL;
	if (strcmp(dest, "-") == 0) {
		out = stdout;
	} else {
		out = fopen(dest, "wb");
		if (!out) {
			fprintf(stderr, "rac: cannot open %s: %s\n", dest, strerror(errno));
			return 1;
		}
	}

	rss_ring_t *ring = rss_ring_open("audio");
	if (!ring) {
		fprintf(stderr, "rac: cannot open audio ring (is RAD running?)\n");
		if (out != stdout)
			fclose(out);
		return 1;
	}

	const rss_ring_header_t *hdr = rss_ring_get_header(ring);
	int codec = hdr->codec;
	int sample_rate = hdr->fps_num; /* audio ring stores sample_rate in fps_num */

	if (out != stdout)
		fprintf(stderr, "rac: recording from audio ring, codec=%d, rate=%d Hz\n", codec,
			sample_rate);

	uint64_t read_seq = 0;
	uint8_t *frame_buf = malloc(hdr->data_size);
	if (!frame_buf) {
		fprintf(stderr, "rac: malloc failed\n");
		rss_ring_close(ring);
		if (out != stdout)
			fclose(out);
		return 1;
	}

	int64_t start_time = rss_timestamp_us();
	int64_t max_us = max_seconds > 0 ? (int64_t)max_seconds * 1000000 : 0;
	uint64_t total_bytes = 0;

	while (g_running) {
		if (max_us > 0 && (rss_timestamp_us() - start_time) >= max_us)
			break;

		int ret = rss_ring_wait(ring, 200);
		if (ret != 0)
			continue;

		uint32_t length = 0;
		rss_ring_slot_t meta;
		ret = rss_ring_read(ring, &read_seq, frame_buf, hdr->data_size, &length, &meta);
		if (ret != 0)
			continue;

		/* Decode to PCM16 LE based on codec */
		if (codec == 0) {
			/* PCMU — decode each byte to int16_t */
			for (uint32_t i = 0; i < length; i++) {
				int16_t sample = ulaw_to_pcm16(frame_buf[i]);
				fwrite(&sample, sizeof(sample), 1, out);
			}
			total_bytes += length * 2;
		} else if (codec == 8) {
			/* PCMA — decode each byte to int16_t */
			for (uint32_t i = 0; i < length; i++) {
				int16_t sample = alaw_to_pcm16(frame_buf[i]);
				fwrite(&sample, sizeof(sample), 1, out);
			}
			total_bytes += length * 2;
		} else if (codec == 11) {
			/* L16 — network byte order PCM16, convert to LE */
			uint16_t *samples = (uint16_t *)frame_buf;
			uint32_t n = length / 2;
			for (uint32_t i = 0; i < n; i++) {
				int16_t s = (int16_t)ntohs(samples[i]);
				fwrite(&s, sizeof(s), 1, out);
			}
			total_bytes += length;
		} else {
			/* Unknown codec — write raw */
			fwrite(frame_buf, 1, length, out);
			total_bytes += length;
		}
	}

	free(frame_buf);
	rss_ring_close(ring);

	if (out != stdout) {
		fclose(out);
		double duration = (double)(rss_timestamp_us() - start_time) / 1000000.0;
		fprintf(stderr, "rac: recorded %.1fs, %llu bytes PCM16\n", duration,
			(unsigned long long)total_bytes);
	}

	return 0;
}

/* ── Play: write PCM16 LE to speaker ring ── */

static int cmd_play(const char *src, int sample_rate)
{
	FILE *in = NULL;
	if (strcmp(src, "-") == 0) {
		in = stdin;
	} else {
		in = fopen(src, "rb");
		if (!in) {
			fprintf(stderr, "rac: cannot open %s: %s\n", src, strerror(errno));
			return 1;
		}
	}

	/* Create the speaker ring (we are the producer, RAD's AO thread is consumer) */
	rss_ring_t *ring = rss_ring_create("speaker", 16, 64 * 1024);
	if (!ring) {
		fprintf(stderr, "rac: failed to create speaker ring\n");
		if (in != stdin)
			fclose(in);
		return 1;
	}
	rss_ring_set_stream_info(ring, 0x11, 0, 0, 0, sample_rate, 1, 0, 0);

	/* Frame size: 20ms at configured sample rate, 16-bit mono */
	int frame_samples = sample_rate / 50;
	int frame_bytes = frame_samples * 2;
	uint8_t *buf = malloc(frame_bytes);
	if (!buf) {
		fprintf(stderr, "rac: malloc failed\n");
		rss_ring_close(ring);
		if (in != stdin)
			fclose(in);
		return 1;
	}

	if (in != stdin)
		fprintf(stderr, "rac: playing to speaker, %d Hz\n", sample_rate);

	uint64_t total_bytes = 0;
	int64_t start_time = rss_timestamp_us();

	while (g_running) {
		size_t n = fread(buf, 1, frame_bytes, in);
		if (n == 0)
			break;

		/* Publish PCM16 LE to speaker ring — RAD's AO thread will read it */
		rss_ring_publish(ring, buf, (uint32_t)n, rss_timestamp_us(), 0, 0);
		total_bytes += n;

		/* Pace output: sleep ~20ms per frame to avoid flooding the ring.
		 * The AO hardware is the real clock; this just prevents ring overflow. */
		usleep(18000);
	}

	free(buf);
	/* Don't destroy — let AO thread drain remaining frames.
	 * Ring persists in /dev/shm until next rac play creates a new one. */
	rss_ring_close(ring);

	if (in != stdin) {
		fclose(in);
		double duration = (double)(rss_timestamp_us() - start_time) / 1000000.0;
		fprintf(stderr, "rac: played %.1fs, %llu bytes\n", duration,
			(unsigned long long)total_bytes);
	}

	return 0;
}

/* ── Status / control commands ── */

static int cmd_ctrl(const char *cmd_json)
{
	char resp[1024] = {0};
	int ret =
		rss_ctrl_send_command("/var/run/rss/rad.sock", cmd_json, resp, sizeof(resp), 2000);
	if (ret < 0) {
		fprintf(stderr, "rac: failed to send to RAD: %s\n",
			ret == -ECONNREFUSED ? "not running" : "connection failed");
		return 1;
	}
	printf("%s\n", resp);
	return 0;
}

/* ── Usage ── */

static void usage(void)
{
	fprintf(stderr, "Usage: rac <command> [options]\n"
			"\n"
			"Commands:\n"
			"  record [options] <file|->   Record mic audio as PCM16 LE\n"
			"    -d <seconds>              Duration limit\n"
			"    -r <rate>                 Expected sample rate (info only)\n"
			"  play [options] <file|->     Play PCM16 LE to speaker\n"
			"    -r <rate>                 Sample rate (default: 16000)\n"
			"  status                      Show audio daemon status\n"
			"  ao-volume <val>             Set speaker volume\n"
			"  ao-gain <val>               Set speaker gain\n");
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		usage();
		return 1;
	}

	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);
	signal(SIGPIPE, SIG_IGN);

	const char *cmd = argv[1];

	if (strcmp(cmd, "record") == 0) {
		int duration = 0;
		int opt;
		optind = 2;
		while ((opt = getopt(argc, argv, "d:r:")) != -1) {
			switch (opt) {
			case 'd':
				duration = atoi(optarg);
				break;
			case 'r':
				/* info only for record */
				break;
			default:
				usage();
				return 1;
			}
		}
		if (optind >= argc) {
			fprintf(stderr, "rac: record requires a destination (file or -)\n");
			return 1;
		}
		return cmd_record(argv[optind], duration);

	} else if (strcmp(cmd, "play") == 0) {
		int sample_rate = 16000;
		int opt;
		optind = 2;
		while ((opt = getopt(argc, argv, "r:")) != -1) {
			switch (opt) {
			case 'r':
				sample_rate = atoi(optarg);
				break;
			default:
				usage();
				return 1;
			}
		}
		if (optind >= argc) {
			fprintf(stderr, "rac: play requires a source (file or -)\n");
			return 1;
		}
		return cmd_play(argv[optind], sample_rate);

	} else if (strcmp(cmd, "status") == 0) {
		return cmd_ctrl("{\"cmd\":\"status\"}");

	} else if (strcmp(cmd, "ao-volume") == 0) {
		if (argc < 3) {
			fprintf(stderr, "Usage: rac ao-volume <value>\n");
			return 1;
		}
		char json[128];
		snprintf(json, sizeof(json), "{\"cmd\":\"ao-set-volume\",\"value\":%s}", argv[2]);
		return cmd_ctrl(json);

	} else if (strcmp(cmd, "ao-gain") == 0) {
		if (argc < 3) {
			fprintf(stderr, "Usage: rac ao-gain <value>\n");
			return 1;
		}
		char json[128];
		snprintf(json, sizeof(json), "{\"cmd\":\"ao-set-gain\",\"value\":%s}", argv[2]);
		return cmd_ctrl(json);

	} else if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "help") == 0) {
		usage();
		return 0;

	} else {
		fprintf(stderr, "rac: unknown command '%s'\n", cmd);
		usage();
		return 1;
	}
}

/*
 * rac.c -- Raptor Audio Client
 *
 * CLI tool for audio input/output:
 *   rac record <file|->         Record mic to file or stdout (PCM16 LE)
 *   rac play <file|->           Play audio to speaker (PCM16, MP3, AAC, Opus)
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

#ifdef RAPTOR_MP3
#include <mp3dec.h>
#endif

#ifdef RAPTOR_AAC
#include <aacdec.h>
#endif

#ifdef RAPTOR_OPUS
#include <opus/opus.h>
#endif

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

/* ── Play helpers ── */

enum play_fmt { FMT_PCM, FMT_MP3, FMT_AAC, FMT_OPUS };

static enum play_fmt detect_format(const char *path, const uint8_t *hdr, size_t hdr_len)
{
	/* Check file extension first */
	if (path) {
		const char *ext = strrchr(path, '.');
		if (ext) {
			if (strcasecmp(ext, ".mp3") == 0)
				return FMT_MP3;
			if (strcasecmp(ext, ".aac") == 0 || strcasecmp(ext, ".adts") == 0)
				return FMT_AAC;
			if (strcasecmp(ext, ".opus") == 0 || strcasecmp(ext, ".ogg") == 0)
				return FMT_OPUS;
			if (strcasecmp(ext, ".pcm") == 0 || strcasecmp(ext, ".raw") == 0 ||
			    strcasecmp(ext, ".wav") == 0)
				return FMT_PCM;
		}
	}
	/* Fall back to magic bytes */
	if (hdr_len >= 4) {
		if (hdr[0] == 0xFF && (hdr[1] & 0xE0) == 0xE0)
			return FMT_MP3; /* MP3 sync or ADTS */
		if (hdr[0] == 'I' && hdr[1] == 'D' && hdr[2] == '3')
			return FMT_MP3;
		if (hdr[0] == 0xFF && (hdr[1] & 0xF6) == 0xF0)
			return FMT_AAC; /* ADTS sync */
		if (hdr[0] == 'O' && hdr[1] == 'g' && hdr[2] == 'g' && hdr[3] == 'S')
			return FMT_OPUS;
	}
	return FMT_PCM;
}

#if defined(RAPTOR_MP3) || defined(RAPTOR_AAC) || defined(RAPTOR_OPUS)
/* Linear interpolation resampler (matches prudynt's resampleLinear) */
static int resample_linear(const int16_t *in, int in_samples, int in_rate, int16_t *out,
			   int out_max, int out_rate)
{
	if (in_rate == out_rate || in_samples == 0) {
		int n = (in_samples < out_max) ? in_samples : out_max;
		memcpy(out, in, n * sizeof(int16_t));
		return n;
	}
	double ratio = (double)out_rate / (double)in_rate;
	int out_samples = (int)(in_samples * ratio + 0.5);
	if (out_samples > out_max)
		out_samples = out_max;
	for (int i = 0; i < out_samples; i++) {
		double pos = (double)i / ratio;
		int base = (int)pos;
		if (base >= in_samples)
			base = in_samples - 1;
		int next = (base + 1 < in_samples) ? base + 1 : base;
		double frac = pos - base;
		double val = in[base] * (1.0 - frac) + in[next] * frac;
		if (val > 32767)
			val = 32767;
		if (val < -32768)
			val = -32768;
		out[i] = (int16_t)val;
	}
	return out_samples;
}

/* Resample (if needed), chunk into 20ms frames, publish to ring */
static void publish_pcm(rss_ring_t *ring, const int16_t *pcm, int samples, int src_rate,
			int dst_rate)
{
	int16_t resamp_buf[8192];
	const int16_t *data = pcm;
	int count = samples;

	if (src_rate != dst_rate) {
		count = resample_linear(pcm, samples, src_rate, resamp_buf,
					(int)(sizeof(resamp_buf) / sizeof(resamp_buf[0])),
					dst_rate);
		data = resamp_buf;
	}

	/* Chunk into 20ms frames at the output rate.
	 * The AO hardware blocks on SendFrame, which is the real playback clock.
	 * We publish slightly ahead to keep the ring fed. */
	int chunk = dst_rate / 50; /* 320 samples at 16kHz = 20ms */
	if (chunk <= 0)
		chunk = 320;
	int off = 0;
	while (off < count) {
		int n = (count - off < chunk) ? (count - off) : chunk;
		rss_ring_publish(ring, (const uint8_t *)(data + off), n * 2, rss_timestamp_us(), 0,
				 0);
		off += n;
		/* Sleep for the frame duration minus a bit to stay ahead of AO */
		usleep((unsigned)(n * 1000000 / dst_rate) - 500);
	}
}
#endif

/* ── Play: decode and write PCM16 to speaker ring ── */

static int cmd_play(const char *src, int sample_rate)
{
	FILE *in = NULL;
	bool is_stdin = strcmp(src, "-") == 0;
	if (is_stdin) {
		in = stdin;
	} else {
		in = fopen(src, "rb");
		if (!in) {
			fprintf(stderr, "rac: cannot open %s: %s\n", src, strerror(errno));
			return 1;
		}
	}

	/* Read header for format detection */
	uint8_t peek[16];
	size_t peek_len = is_stdin ? 0 : fread(peek, 1, sizeof(peek), in);
	enum play_fmt fmt = is_stdin ? FMT_PCM : detect_format(src, peek, peek_len);

	const char *fmt_str[] = {"pcm", "mp3", "aac", "opus"};
	if (!is_stdin)
		fprintf(stderr, "rac: playing %s (%s), %d Hz\n", src, fmt_str[fmt], sample_rate);

	/* Rewind after peek */
	if (peek_len > 0)
		fseek(in, 0, SEEK_SET);

	/* Create speaker ring */
	rss_ring_t *ring = rss_ring_create("speaker", 16, 64 * 1024);
	if (!ring) {
		fprintf(stderr, "rac: failed to create speaker ring\n");
		if (!is_stdin)
			fclose(in);
		return 1;
	}
	rss_ring_set_stream_info(ring, 0x11, 0, 0, 0, sample_rate, 1, 0, 0);

	int ret = 0;
	int64_t start_time = rss_timestamp_us();
	uint64_t total_samples = 0;

	if (fmt == FMT_PCM) {
		/* Raw PCM16 LE — passthrough */
		int frame_bytes = (sample_rate / 50) * 2;
		uint8_t *buf = malloc(frame_bytes);
		if (!buf) {
			ret = 1;
			goto done;
		}
		while (g_running) {
			size_t n = fread(buf, 1, frame_bytes, in);
			if (n == 0)
				break;
			rss_ring_publish(ring, buf, (uint32_t)n, rss_timestamp_us(), 0, 0);
			total_samples += n / 2;
			usleep(18000);
		}
		free(buf);
	}
#ifdef RAPTOR_MP3
	else if (fmt == FMT_MP3) {
		HMP3Decoder mp3 = MP3InitDecoder();
		if (!mp3) {
			fprintf(stderr, "rac: MP3InitDecoder failed\n");
			ret = 1;
			goto done;
		}
		/* Read entire file into memory (MP3 files are small) */
		fseek(in, 0, SEEK_END);
		long fsize = ftell(in);
		fseek(in, 0, SEEK_SET);
		uint8_t *mp3_buf = malloc(fsize);
		if (!mp3_buf) {
			MP3FreeDecoder(mp3);
			ret = 1;
			goto done;
		}
		fread(mp3_buf, 1, fsize, in);

		int16_t pcm_buf[2304]; /* max MP3 frame: 1152 samples * 2 channels */
		unsigned char *read_ptr = mp3_buf;
		int bytes_left = (int)fsize;

		while (g_running && bytes_left > 0) {
			int offset = MP3FindSyncWord(read_ptr, bytes_left);
			if (offset < 0)
				break;
			read_ptr += offset;
			bytes_left -= offset;

			int err = MP3Decode(mp3, &read_ptr, &bytes_left, pcm_buf, 0);
			if (err) {
				if (err == -6) /* ERR_MP3_INDATA_UNDERFLOW */
					break;
				continue; /* skip bad frame */
			}
			MP3FrameInfo info;
			MP3GetLastFrameInfo(mp3, &info);

			/* Downmix stereo to mono if needed */
			int samples = info.outputSamps / info.nChans;
			if (info.nChans == 2) {
				for (int i = 0; i < samples; i++)
					pcm_buf[i] = (pcm_buf[i * 2] + pcm_buf[i * 2 + 1]) / 2;
			}
			publish_pcm(ring, pcm_buf, samples, info.samprate, sample_rate);
			total_samples += samples;
		}
		free(mp3_buf);
		MP3FreeDecoder(mp3);
	}
#endif
#ifdef RAPTOR_AAC
	else if (fmt == FMT_AAC) {
		HAACDecoder aac = AACInitDecoder();
		if (!aac) {
			fprintf(stderr, "rac: AACInitDecoder failed\n");
			ret = 1;
			goto done;
		}
		/* Read entire file */
		fseek(in, 0, SEEK_END);
		long fsize = ftell(in);
		fseek(in, 0, SEEK_SET);
		uint8_t *aac_buf = malloc(fsize);
		if (!aac_buf) {
			AACFreeDecoder(aac);
			ret = 1;
			goto done;
		}
		fread(aac_buf, 1, fsize, in);

		int16_t pcm_buf[4096]; /* max AAC frame: 2048 samples * 2 channels */
		unsigned char *read_ptr = aac_buf;
		int bytes_left = (int)fsize;

		while (g_running && bytes_left > 0) {
			int offset = AACFindSyncWord(read_ptr, bytes_left);
			if (offset < 0)
				break;
			read_ptr += offset;
			bytes_left -= offset;

			int err = AACDecode(aac, &read_ptr, &bytes_left, pcm_buf);
			if (err) {
				if (bytes_left <= 0)
					break;
				continue;
			}
			AACFrameInfo info;
			AACGetLastFrameInfo(aac, &info);

			int samples = info.outputSamps / info.nChans;
			if (info.nChans == 2) {
				for (int i = 0; i < samples; i++)
					pcm_buf[i] = (pcm_buf[i * 2] + pcm_buf[i * 2 + 1]) / 2;
			}
			publish_pcm(ring, pcm_buf, samples, info.sampRateOut, sample_rate);
			total_samples += samples;
		}
		free(aac_buf);
		AACFreeDecoder(aac);
	}
#endif
#ifdef RAPTOR_OPUS
	else if (fmt == FMT_OPUS) {
		/* Opus in Ogg container — simplified parser */
		fprintf(stderr, "rac: Opus/Ogg playback not yet implemented\n");
		ret = 1;
		goto done;
	}
#endif
	else {
		fprintf(stderr, "rac: unsupported format (compile with MP3/AAC/OPUS support)\n");
		ret = 1;
	}

done:
	/* Let AO thread drain remaining frames, then destroy so it reconnects next time */
	usleep(100000);
	rss_ring_destroy(ring);
	if (!is_stdin) {
		fclose(in);
		double duration = (double)(rss_timestamp_us() - start_time) / 1000000.0;
		fprintf(stderr, "rac: played %.1fs, %llu samples\n", duration,
			(unsigned long long)total_samples);
	}
	return ret;
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
		if (sample_rate <= 0) {
			fprintf(stderr, "rac: invalid sample rate\n");
			return 1;
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

/*
 * rac_play.c -- Raptor Audio Client: playback
 *
 * Decode audio (PCM, MP3, AAC, Opus) and publish to speaker ring.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>

#include "rac.h"

#ifdef RAPTOR_MP3
#include <mp3dec.h>
#endif

#ifdef RAPTOR_AAC
#include <aacdec.h>
#endif

#ifdef RAPTOR_OPUS
#include <opus/opus.h>
#endif

/* ── Play helpers ── */

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

int cmd_play(const char *src, int sample_rate)
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

	/* Tell RAD to flush stale hardware audio and prepare for new playback */
	{
		char resp[256];
		rss_ctrl_send_command("/var/run/rss/rad.sock", "{\"cmd\":\"ao-flush\"}",
				      resp, sizeof(resp), 500);
	}

	/* Create speaker ring */
	rss_ring_t *ring = rss_ring_create("speaker", 16, 64 * 1024);
	if (!ring) {
		fprintf(stderr, "rac: failed to create speaker ring\n");
		if (!is_stdin)
			fclose(in);
		return 1;
	}
	rss_ring_set_stream_info(ring, 0x11, 0, 0, 0, sample_rate, 1, 0, 0);

	/* Wait for AO thread to attach — prevents beginning cutoff */
	for (int i = 0; i < 40 && g_running; i++) {
		if (rss_ring_reader_count(ring) > 0)
			break;
		usleep(5000);
	}

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
		/* Read entire Ogg/Opus file */
		fseek(in, 0, SEEK_END);
		long fsize = ftell(in);
		fseek(in, 0, SEEK_SET);
		uint8_t *ogg_buf = malloc(fsize);
		if (!ogg_buf) {
			ret = 1;
			goto done;
		}
		fread(ogg_buf, 1, fsize, in);

		int opus_err;
		/* Opus always decodes at 48kHz internally */
		/* Decode at target rate — Opus supports 8/12/16/24/48kHz natively */
		int opus_rate = sample_rate;
		if (opus_rate != 8000 && opus_rate != 12000 && opus_rate != 16000 &&
		    opus_rate != 24000 && opus_rate != 48000)
			opus_rate = 48000; /* fallback to 48kHz if non-standard */
		OpusDecoder *opus_dec = opus_decoder_create(opus_rate, 1, &opus_err);
		if (opus_err != OPUS_OK || !opus_dec) {
			fprintf(stderr, "rac: opus_decoder_create failed: %d\n", opus_err);
			free(ogg_buf);
			ret = 1;
			goto done;
		}

		int16_t pcm_buf[5760]; /* max Opus frame: 120ms at 48kHz */
		uint8_t *p = ogg_buf;
		uint8_t *end = ogg_buf + fsize;
		int page_count = 0;

		while (g_running && p + 27 <= end) {
			/* Ogg page header: "OggS" + version + flags + ... */
			if (p[0] != 'O' || p[1] != 'g' || p[2] != 'g' || p[3] != 'S') {
				p++;
				continue;
			}
			uint8_t segments = p[26];
			if (p + 27 + segments > end)
				break;

			/* Segment table starts at offset 27 */
			uint8_t *seg_table = p + 27;
			int body_size = 0;
			for (int i = 0; i < segments; i++)
				body_size += seg_table[i];

			uint8_t *body = seg_table + segments;
			if (body + body_size > end)
				break;

			page_count++;
			/* Skip first 2 pages (OpusHead + OpusTags) */
			if (page_count <= 2) {
				p = body + body_size;
				continue;
			}

			/* Extract packets from segments */
			uint8_t *pkt = body;
			int pkt_len = 0;
			for (int i = 0; i < segments; i++) {
				pkt_len += seg_table[i];
				if (seg_table[i] < 255) {
					/* Complete packet */
					if (pkt_len > 0) {
						int decoded = opus_decode(opus_dec, pkt, pkt_len,
									  pcm_buf, 5760, 0);
						if (decoded > 0) {
							publish_pcm(ring, pcm_buf, decoded,
								    opus_rate, sample_rate);
							total_samples += decoded;
						}
					}
					pkt += pkt_len;
					pkt_len = 0;
				}
			}
			/* Handle final packet if last segment was 255 (spanning) */
			if (pkt_len > 0) {
				int decoded = opus_decode(opus_dec, pkt, pkt_len, pcm_buf, 5760, 0);
				if (decoded > 0) {
					publish_pcm(ring, pcm_buf, decoded, opus_rate, sample_rate);
					total_samples += decoded;
				}
			}

			p = body + body_size;
		}

		opus_decoder_destroy(opus_dec);
		free(ogg_buf);
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

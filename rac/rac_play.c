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

/*
 * Deadline-based realtime pacer. start_us is captured at init; each
 * pacer_advance() projects an output deadline from cumulative samples
 * and sample_rate, then sleeps until that deadline. This stops the
 * producer from drifting ahead of realtime over long files, which would
 * otherwise leave a tail in the ring at EOF.
 */
typedef struct {
	int64_t start_us;
	uint64_t samples;
	int sample_rate;
} rac_pacer_t;

static void pacer_init(rac_pacer_t *p, int sample_rate)
{
	p->start_us = rss_timestamp_us();
	p->samples = 0;
	p->sample_rate = sample_rate;
}

static void pacer_advance(rac_pacer_t *p, int samples)
{
	p->samples += (uint64_t)samples;
	int64_t deadline = p->start_us + (int64_t)p->samples * 1000000 / p->sample_rate;
	int64_t now = rss_timestamp_us();
	if (deadline > now)
		usleep((unsigned)(deadline - now));
}

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
	/* Fall back to magic bytes. Detection order matters here: ADTS-AAC
	 * and MP3 share the 0xFF sync byte, and the MP3 sync mask (top 11
	 * bits = 1) is broader than ADTS (top 12 bits = 1 AND layer=00).
	 * Check the stricter ADTS pattern first so ADTS streams don't get
	 * misdetected as MP3. */
	if (hdr_len >= 4) {
		if (hdr[0] == 'O' && hdr[1] == 'g' && hdr[2] == 'g' && hdr[3] == 'S')
			return FMT_OPUS;
		if (hdr[0] == 'I' && hdr[1] == 'D' && hdr[2] == '3')
			return FMT_MP3;
		if (hdr[0] == 0xFF && (hdr[1] & 0xF6) == 0xF0)
			return FMT_AAC; /* ADTS sync: 12 bits of 1 + layer=00 */
		if (hdr[0] == 0xFF && (hdr[1] & 0xE0) == 0xE0)
			return FMT_MP3; /* MP3 sync: 11 bits of 1, any layer */
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
static void publish_pcm(rss_ring_t *ring, rac_pacer_t *pacer, const int16_t *pcm, int samples,
			int src_rate, int dst_rate)
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

	int chunk = dst_rate / 50; /* 320 samples at 16kHz = 20ms */
	if (chunk <= 0)
		chunk = 320;
	int off = 0;
	while (off < count) {
		int n = (count - off < chunk) ? (count - off) : chunk;
		rss_ring_publish(ring, (const uint8_t *)(data + off), n * 2, rss_timestamp_us(), 0,
				 0);
		off += n;
		pacer_advance(pacer, n);
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

	/* Read header for format detection. fread works on pipes too, so
	 * this path serves both file and stdin input. We can't fseek back
	 * on a pipe, so the peeked bytes are kept here and prepended to
	 * the decoder buffer below. */
	uint8_t peek[16];
	size_t peek_len = fread(peek, 1, sizeof(peek), in);
	enum play_fmt fmt = detect_format(is_stdin ? NULL : src, peek, peek_len);

	const char *fmt_str[] = {"pcm", "mp3", "aac", "opus"};
	fprintf(stderr, "rac: playing %s (%s), %d Hz\n", is_stdin ? "<stdin>" : src, fmt_str[fmt],
		sample_rate);

	/* For a regular file we can rewind and let the decoder slurp the
	 * whole thing. For stdin/pipe we can't — those paths read
	 * incrementally and use `peek` as the initial seed. */
	bool can_rewind = !is_stdin && peek_len > 0;
	if (can_rewind)
		fseek(in, 0, SEEK_SET);

	/* Tell RAD to flush stale hardware audio and prepare for new playback */
	{
		char resp[256];
		rss_ctrl_send_command(RSS_RUN_DIR "/rad.sock", "{\"cmd\":\"ao-flush\"}", resp,
				      sizeof(resp), 500);
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
	rac_pacer_t pacer;
	pacer_init(&pacer, sample_rate);

	if (fmt == FMT_PCM) {
		/* Raw PCM16 LE — passthrough */
		int frame_bytes = (sample_rate / 50) * 2;
		uint8_t *buf = malloc(frame_bytes);
		if (!buf) {
			ret = 1;
			goto done;
		}
		/* Emit the peeked bytes first (for stdin, where we can't
		 * rewind). Then stream the rest. */
		if (!can_rewind && peek_len > 0) {
			rss_ring_publish(ring, peek, (uint32_t)peek_len, rss_timestamp_us(), 0, 0);
			total_samples += peek_len / 2;
			pacer_advance(&pacer, (int)(peek_len / 2));
		}
		while (g_running) {
			size_t n = fread(buf, 1, frame_bytes, in);
			if (n == 0)
				break;
			rss_ring_publish(ring, buf, (uint32_t)n, rss_timestamp_us(), 0, 0);
			total_samples += n / 2;
			pacer_advance(&pacer, (int)(n / 2));
		}
		free(buf);
	}
#ifdef RAPTOR_MP3
	else if (fmt == FMT_MP3) {
		/*
		 * Streaming MP3 decoder — handles both file and pipe input.
		 * buf is a refillable window; MP3Decode advances read_ptr as
		 * it consumes, we keep the unconsumed tail and refill from
		 * the input.
		 *
		 * 64 KB is enough headroom: the largest MP3 frame is ~1.5 KB
		 * (320 kbps MPEG-1 L3), so we can always hold a full frame
		 * plus a healthy refill margin. MAX_FRAME_BYTES controls when
		 * we pull more data.
		 */
		enum { MP3_BUF = 64 * 1024, MP3_REFILL_THRESH = 4 * 1024 };
		HMP3Decoder mp3 = MP3InitDecoder();
		if (!mp3) {
			fprintf(stderr, "rac: MP3InitDecoder failed\n");
			ret = 1;
			goto done;
		}
		uint8_t *buf = malloc(MP3_BUF);
		if (!buf) {
			MP3FreeDecoder(mp3);
			ret = 1;
			goto done;
		}

		/* Seed with the peek (which we already consumed from `in`)
		 * and then top up from the stream. */
		int bytes_left = 0;
		if (peek_len > 0 && !can_rewind) {
			memcpy(buf, peek, peek_len);
			bytes_left = (int)peek_len;
		}
		size_t n = fread(buf + bytes_left, 1, (size_t)(MP3_BUF - bytes_left), in);
		bytes_left += (int)n;

		int16_t pcm_buf[2304]; /* max MP3 frame: 1152 samples * 2 channels */
		bool eof = false;

		while (g_running && bytes_left > 0) {
			/* Refill when running low so MP3Decode always sees at
			 * least a full max-size frame plus headroom. The
			 * unconsumed tail is already at buf[0..bytes_left]
			 * (compacted after each successful decode below), so
			 * just fread onto the end. */
			if (!eof && bytes_left < MP3_REFILL_THRESH) {
				n = fread(buf + bytes_left, 1, (size_t)(MP3_BUF - bytes_left), in);
				if (n == 0)
					eof = true;
				bytes_left += (int)n;
			}

			int offset = MP3FindSyncWord(buf, bytes_left);
			if (offset < 0) {
				if (eof)
					break;
				/* No sync in this window — drain and refill. */
				bytes_left = 0;
				continue;
			}
			if (offset > 0) {
				memmove(buf, buf + offset, (size_t)(bytes_left - offset));
				bytes_left -= offset;
			}

			unsigned char *read_ptr = buf;
			int err = MP3Decode(mp3, &read_ptr, &bytes_left, pcm_buf, 0);
			if (err) {
				if (err == -6) /* INDATA_UNDERFLOW — need more */
					continue;
				/* Bad frame: skip the sync byte and retry. */
				if (bytes_left > 1) {
					memmove(buf, buf + 1, (size_t)(bytes_left - 1));
					bytes_left--;
				} else {
					bytes_left = 0;
				}
				continue;
			}
			/* MP3Decode leaves read_ptr past the consumed frame
			 * and bytes_left set to whatever remains. Shift the
			 * tail back to buf start for the next refill. */
			if (bytes_left > 0 && read_ptr != buf)
				memmove(buf, read_ptr, (size_t)bytes_left);

			MP3FrameInfo info;
			MP3GetLastFrameInfo(mp3, &info);

			int samples = info.outputSamps / info.nChans;
			if (info.nChans == 2) {
				for (int i = 0; i < samples; i++)
					pcm_buf[i] = (pcm_buf[i * 2] + pcm_buf[i * 2 + 1]) / 2;
			}
			publish_pcm(ring, &pacer, pcm_buf, samples, info.samprate, sample_rate);
			total_samples += samples;
		}
		free(buf);
		MP3FreeDecoder(mp3);
	}
#endif
#ifdef RAPTOR_AAC
	else if (fmt == FMT_AAC) {
		/*
		 * Streaming ADTS-AAC decoder (same pattern as MP3 above).
		 * Max ADTS frame ~8 KB (2048 samples * 2 ch); 16 KB buffer
		 * gives plenty of headroom.
		 */
		enum { AAC_BUF = 16 * 1024, AAC_REFILL_THRESH = 4 * 1024 };
		HAACDecoder aac = AACInitDecoder();
		if (!aac) {
			fprintf(stderr, "rac: AACInitDecoder failed\n");
			ret = 1;
			goto done;
		}
		uint8_t *buf = malloc(AAC_BUF);
		if (!buf) {
			AACFreeDecoder(aac);
			ret = 1;
			goto done;
		}

		int bytes_left = 0;
		if (peek_len > 0 && !can_rewind) {
			memcpy(buf, peek, peek_len);
			bytes_left = (int)peek_len;
		}
		size_t n = fread(buf + bytes_left, 1, (size_t)(AAC_BUF - bytes_left), in);
		bytes_left += (int)n;

		int16_t pcm_buf[4096]; /* max AAC frame: 2048 samples * 2 ch */
		bool eof = false;

		while (g_running && bytes_left > 0) {
			if (!eof && bytes_left < AAC_REFILL_THRESH) {
				n = fread(buf + bytes_left, 1, (size_t)(AAC_BUF - bytes_left), in);
				if (n == 0)
					eof = true;
				bytes_left += (int)n;
			}

			int offset = AACFindSyncWord(buf, bytes_left);
			if (offset < 0) {
				if (eof)
					break;
				bytes_left = 0;
				continue;
			}
			if (offset > 0) {
				memmove(buf, buf + offset, (size_t)(bytes_left - offset));
				bytes_left -= offset;
			}

			unsigned char *read_ptr = buf;
			int err = AACDecode(aac, &read_ptr, &bytes_left, pcm_buf);
			if (err) {
				/* Helix AAC returns -1 on underflow — treat any
				 * error as "skip sync byte and retry" to stay
				 * conservative; refill will bring more data. */
				if (bytes_left > 1) {
					memmove(buf, buf + 1, (size_t)(bytes_left - 1));
					bytes_left--;
				} else {
					bytes_left = 0;
				}
				continue;
			}
			if (bytes_left > 0 && read_ptr != buf)
				memmove(buf, read_ptr, (size_t)bytes_left);

			AACFrameInfo info;
			AACGetLastFrameInfo(aac, &info);

			int samples = info.outputSamps / info.nChans;
			if (info.nChans == 2) {
				for (int i = 0; i < samples; i++)
					pcm_buf[i] = (pcm_buf[i * 2] + pcm_buf[i * 2 + 1]) / 2;
			}
			publish_pcm(ring, &pacer, pcm_buf, samples, info.sampRateOut, sample_rate);
			total_samples += samples;
		}
		free(buf);
		AACFreeDecoder(aac);
	}
#endif
#ifdef RAPTOR_OPUS
	else if (fmt == FMT_OPUS) {
		/*
		 * Streaming Ogg/Opus decoder. Each Ogg page has a 27-byte
		 * fixed header followed by a variable segment table and
		 * body. Max page size is 27 + 255 + 255*255 = 65307 bytes,
		 * so the working buffer must be at least that; 96 KB leaves
		 * refill headroom.
		 *
		 * Packets can span multiple segments within a page (last
		 * segment == 255 flag). We follow upstream libopus examples
		 * and assume packets do not span pages — Ogg permits it, but
		 * opus streams in practice keep one packet per page group.
		 */
		enum { OPUS_BUF = 96 * 1024, OPUS_REFILL_THRESH = 65 * 1024 };

		int opus_err;
		int opus_rate = sample_rate;
		if (opus_rate != 8000 && opus_rate != 12000 && opus_rate != 16000 &&
		    opus_rate != 24000 && opus_rate != 48000)
			opus_rate = 48000; /* fallback to 48kHz if non-standard */
		OpusDecoder *opus_dec = opus_decoder_create(opus_rate, 1, &opus_err);
		if (opus_err != OPUS_OK || !opus_dec) {
			fprintf(stderr, "rac: opus_decoder_create failed: %d\n", opus_err);
			ret = 1;
			goto done;
		}

		uint8_t *buf = malloc(OPUS_BUF);
		if (!buf) {
			opus_decoder_destroy(opus_dec);
			ret = 1;
			goto done;
		}

		int bytes_left = 0;
		if (peek_len > 0 && !can_rewind) {
			memcpy(buf, peek, peek_len);
			bytes_left = (int)peek_len;
		}
		size_t n = fread(buf + bytes_left, 1, (size_t)(OPUS_BUF - bytes_left), in);
		bytes_left += (int)n;

		int16_t pcm_buf[5760]; /* max Opus frame: 120 ms at 48 kHz */
		int page_count = 0;
		bool eof = false;

		while (g_running && bytes_left > 0) {
			if (!eof && bytes_left < OPUS_REFILL_THRESH) {
				n = fread(buf + bytes_left, 1, (size_t)(OPUS_BUF - bytes_left), in);
				if (n == 0)
					eof = true;
				bytes_left += (int)n;
			}

			/* Need at least the 27-byte fixed Ogg header before
			 * we can tell how big the page is. */
			if (bytes_left < 27) {
				if (eof)
					break;
				continue;
			}
			if (buf[0] != 'O' || buf[1] != 'g' || buf[2] != 'g' || buf[3] != 'S') {
				/* Drop one byte and resync. */
				memmove(buf, buf + 1, (size_t)(bytes_left - 1));
				bytes_left--;
				continue;
			}

			uint8_t segments = buf[26];
			int page_size;
			if (bytes_left < 27 + segments) {
				if (eof)
					break;
				continue; /* refill */
			}
			uint8_t *seg_table = buf + 27;
			int body_size = 0;
			for (int i = 0; i < segments; i++)
				body_size += seg_table[i];
			page_size = 27 + segments + body_size;
			if (bytes_left < page_size) {
				if (eof)
					break;
				continue; /* refill */
			}

			page_count++;
			uint8_t *body = seg_table + segments;

			/* First two pages are OpusHead + OpusTags — no audio. */
			if (page_count > 2) {
				uint8_t *pkt = body;
				int pkt_len = 0;
				for (int i = 0; i < segments; i++) {
					pkt_len += seg_table[i];
					if (seg_table[i] < 255) {
						if (pkt_len > 0) {
							int decoded =
								opus_decode(opus_dec, pkt, pkt_len,
									    pcm_buf, 5760, 0);
							if (decoded > 0) {
								publish_pcm(ring, &pacer, pcm_buf,
									    decoded, opus_rate,
									    sample_rate);
								total_samples += decoded;
							}
						}
						pkt += pkt_len;
						pkt_len = 0;
					}
				}
				/* Trailing packet (seg==255 continuation that
				 * ends at page boundary — rare in practice for
				 * opus radio streams). */
				if (pkt_len > 0) {
					int decoded = opus_decode(opus_dec, pkt, pkt_len, pcm_buf,
								  5760, 0);
					if (decoded > 0) {
						publish_pcm(ring, &pacer, pcm_buf, decoded,
							    opus_rate, sample_rate);
						total_samples += decoded;
					}
				}
			}

			/* Consume this page from the buffer. */
			memmove(buf, buf + page_size, (size_t)(bytes_left - page_size));
			bytes_left -= page_size;
		}

		free(buf);
		opus_decoder_destroy(opus_dec);
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
	if (!is_stdin)
		fclose(in);
	double duration = (double)(rss_timestamp_us() - start_time) / 1000000.0;
	fprintf(stderr, "rac: played %.1fs, %llu samples\n", duration,
		(unsigned long long)total_samples);
	return ret;
}

/*
 * test_mux.c -- Host-side test for the fMP4 muxer.
 *
 * Writes a test MP4 file with fake H.264 video + PCM audio,
 * then validates with ffprobe.
 *
 * Build: gcc -O1 -g -I../rmr -o test_mux test_mux.c ../rmr/rmr_mux.c
 * Run:   ./test_mux && ffprobe -v error -show_format -show_streams /tmp/test_raptor.mp4
 */

#include "rmr_mux.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *g_fp;

static int file_write(const void *buf, uint32_t len, void *ctx)
{
	(void)ctx;
	return fwrite(buf, 1, len, g_fp) == len ? 0 : -1;
}

/* Minimal H.264 SPS (Baseline, 640x480, level 3.0) */
static const uint8_t test_sps[] = {
	0x67, 0x42, 0xC0, 0x1E, 0xD9, 0x00, 0xA0, 0x47, 0xFE, 0xC8,
};

/* Minimal H.264 PPS */
static const uint8_t test_pps[] = {
	0x68,
	0xCE,
	0x38,
	0x80,
};

/* Fake IDR NAL (AVCC format: 4-byte length prefix + payload) */
static uint8_t *make_fake_nal(uint32_t payload_size, uint8_t nal_type, uint32_t *out_size)
{
	*out_size = 4 + payload_size;
	uint8_t *buf = malloc(*out_size);
	if (!buf)
		return NULL;
	buf[0] = (payload_size >> 24) & 0xFF;
	buf[1] = (payload_size >> 16) & 0xFF;
	buf[2] = (payload_size >> 8) & 0xFF;
	buf[3] = payload_size & 0xFF;
	buf[4] = nal_type;
	memset(buf + 5, 0xAB, payload_size - 1); /* fake data */
	return buf;
}

int main(void)
{
	const char *path = "/tmp/test_raptor.mp4";
	g_fp = fopen(path, "wb");
	if (!g_fp) {
		perror("fopen");
		return 1;
	}

	rmr_mux_t *mux = rmr_mux_create(file_write, NULL);

	rmr_video_params_t vp = {
		.codec = RMR_CODEC_H264,
		.width = 640,
		.height = 480,
		.timescale = 90000,
	};

	rmr_audio_params_t ap = {
		.codec = RMR_AUDIO_L16,
		.sample_rate = 16000,
		.channels = 1,
		.bits_per_sample = 16,
	};

	rmr_mux_set_video(mux, &vp, test_sps, sizeof(test_sps), test_pps, sizeof(test_pps), NULL,
			  0);
	rmr_mux_set_audio(mux, &ap);
	rmr_mux_start(mux);

	/* Write 3 GOPs (3 fragments) at 25fps, GOP=25 */
	int64_t v_dts = 0;
	int64_t a_dts = 0;
	uint32_t v_inc = 90000 / 25;	    /* 3600 */
	uint32_t a_samples_per_frame = 320; /* 20ms at 16kHz */

	for (int gop = 0; gop < 3; gop++) {
		for (int frame = 0; frame < 25; frame++) {
			bool is_key = (frame == 0);
			uint8_t nal_type = is_key ? 0x65 : 0x41; /* IDR or SLICE */
			uint32_t nal_size;
			uint8_t *nal = make_fake_nal(is_key ? 4096 : 1024, nal_type, &nal_size);

			rmr_video_sample_t vs = {
				.data = nal,
				.size = nal_size,
				.dts = v_dts,
				.pts = v_dts,
				.is_key = is_key,
			};
			rmr_mux_write_video(mux, &vs);
			free(nal);
			v_dts += v_inc;

			/* ~2 audio frames per video frame at 16kHz/20ms vs 25fps/40ms */
			for (int a = 0; a < 2; a++) {
				uint8_t audio[640];
				memset(audio, 0x80, sizeof(audio));
				rmr_audio_sample_t as = {
					.data = audio,
					.size = sizeof(audio),
					.dts = a_dts,
				};
				rmr_mux_write_audio(mux, &as);
				a_dts += a_samples_per_frame;
			}
		}

		/* Flush at end of each GOP */
		rmr_mux_flush_fragment(mux);
	}

	rmr_mux_finalize(mux);
	rmr_mux_destroy(mux);
	fclose(g_fp);

	printf("Wrote %s\n", path);
	printf("Validate with: ffprobe -v error -show_format -show_streams %s\n", path);
	return 0;
}

/*
 * create_rings.c -- Create dummy SHM rings for host-side ASan testing.
 * Writes a few fake JPEG frames so RHD/RSD have data to serve.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <rss_ipc.h>
#include <rss_common.h>

static volatile sig_atomic_t g_running = 1;

static void sighandler(int sig)
{
	(void)sig;
	g_running = 0;
}

/* Minimal valid JPEG: SOI + APP0 + EOI */
static const uint8_t fake_jpeg[] = {
	0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00,
	0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0xFF, 0xD9,
};

int main(void)
{
	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);

	rss_log_init("create_rings", RSS_LOG_INFO, RSS_LOG_TARGET_STDERR, NULL);

	/* Create video rings */
	rss_ring_t *main_ring = rss_ring_create("main", 16, 2 * 1024 * 1024);
	if (main_ring)
		rss_ring_set_stream_info(main_ring, 0, 0, 1920, 1080, 25, 1, 100, 40);

	rss_ring_t *sub_ring = rss_ring_create("sub", 16, 512 * 1024);
	if (sub_ring)
		rss_ring_set_stream_info(sub_ring, 1, 0, 640, 360, 25, 1, 66, 30);

	/* Create JPEG rings */
	rss_ring_t *jpeg0 = rss_ring_create("jpeg0", 4, 512 * 1024);
	if (jpeg0)
		rss_ring_set_stream_info(jpeg0, 32, 2, 1920, 1080, 1, 1, 0, 0);

	rss_ring_t *jpeg1 = rss_ring_create("jpeg1", 4, 64 * 1024);
	if (jpeg1)
		rss_ring_set_stream_info(jpeg1, 33, 2, 640, 360, 1, 1, 0, 0);

	/* Create audio ring */
	rss_ring_t *audio = rss_ring_create("audio", 64, 256 * 1024);
	if (audio)
		rss_ring_set_stream_info(audio, 64, 11, 0, 0, 16000, 1, 0, 0);

	RSS_INFO("created rings: main sub jpeg0 jpeg1 audio");

	/* Publish fake JPEG frames so RHD has data */
	rss_iov_t iov = {.data = fake_jpeg, .length = sizeof(fake_jpeg)};
	if (jpeg0)
		for (int i = 0; i < 4; i++)
			rss_ring_publish_iov(jpeg0, &iov, 1, i * 40000, 0x30, 1);
	if (jpeg1)
		for (int i = 0; i < 4; i++)
			rss_ring_publish_iov(jpeg1, &iov, 1, i * 40000, 0x30, 1);

	RSS_INFO("published fake JPEG frames");

	/* Continuously publish fake H.264 to main ring at ~25fps.
	 * IDR every 50 frames (2 seconds). Annex B format with
	 * start codes, matching real encoder output. */
	uint8_t idr_frame[4096];
	uint8_t p_frame[512];

	/* IDR: [start_code][SPS][start_code][PPS][start_code][IDR_slice] */
	static const uint8_t sps[] = {0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xC0, 0x1E,
				      0xD9, 0x00, 0xA0, 0x47, 0xFE, 0xC8};
	static const uint8_t pps[] = {0x00, 0x00, 0x00, 0x01, 0x68, 0xCE, 0x38, 0x80};
	uint32_t idr_off = 0;
	memcpy(idr_frame, sps, sizeof(sps));
	idr_off += sizeof(sps);
	memcpy(idr_frame + idr_off, pps, sizeof(pps));
	idr_off += sizeof(pps);
	idr_frame[idr_off++] = 0x00;
	idr_frame[idr_off++] = 0x00;
	idr_frame[idr_off++] = 0x00;
	idr_frame[idr_off++] = 0x01;
	idr_frame[idr_off++] = 0x65; /* IDR slice */
	memset(idr_frame + idr_off, 0xAB, sizeof(idr_frame) - idr_off);
	uint32_t idr_len = sizeof(idr_frame);

	/* P-frame: [start_code][P_slice] */
	p_frame[0] = 0x00;
	p_frame[1] = 0x00;
	p_frame[2] = 0x00;
	p_frame[3] = 0x01;
	p_frame[4] = 0x41; /* P-slice */
	memset(p_frame + 5, 0xCD, sizeof(p_frame) - 5);
	uint32_t p_len = sizeof(p_frame);

	/* Audio frame (320 samples * 2 bytes = 640 bytes, 20ms at 16kHz) */
	uint8_t audio_frame[640];
	memset(audio_frame, 0x80, sizeof(audio_frame));

	int64_t ts = 0;
	int64_t ats = 0;
	uint32_t frame_num = 0;

	RSS_INFO("publishing fake H.264+audio at 25fps...");

	while (g_running) {
		bool is_key = (frame_num % 50 == 0);
		const uint8_t *data = is_key ? idr_frame : p_frame;
		uint32_t len = is_key ? idr_len : p_len;
		uint16_t nal = is_key ? 0x13 : 0x14; /* RSS_NAL_H264_IDR / SLICE */

		rss_iov_t viov = {.data = data, .length = len};
		if (main_ring)
			rss_ring_publish_iov(main_ring, &viov, 1, ts, nal, is_key ? 1 : 0);

		/* 2 audio frames per video frame */
		rss_iov_t aiov = {.data = audio_frame, .length = sizeof(audio_frame)};
		if (audio)
			for (int a = 0; a < 2; a++) {
				rss_ring_publish_iov(audio, &aiov, 1, ats, 11, 0);
				ats += 320; /* 320 samples = 20ms */
			}

		ts += 40000; /* 40ms = 25fps in microseconds */
		frame_num++;

		usleep(40000); /* ~25fps */
	}

	/* Cleanup */
	if (main_ring)
		rss_ring_destroy(main_ring);
	if (sub_ring)
		rss_ring_destroy(sub_ring);
	if (jpeg0)
		rss_ring_destroy(jpeg0);
	if (jpeg1)
		rss_ring_destroy(jpeg1);
	if (audio)
		rss_ring_destroy(audio);

	RSS_INFO("rings destroyed");
	return 0;
}

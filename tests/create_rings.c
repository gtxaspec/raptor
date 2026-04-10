/*
 * create_rings.c -- Create dummy SHM rings for host-side ASan testing.
 *
 * Publishes fake H.264 video, JPEG snapshots, and audio in a continuous
 * loop to exercise all consumer daemons (RSD, RHD, RMR, RWD).
 *
 * Usage:
 *   create_rings              # L16 audio (default)
 *   create_rings pcmu         # G.711 mu-law audio
 *   create_rings pcma         # G.711 A-law audio
 *   create_rings aac          # fake AAC frames
 *   create_rings opus         # fake Opus packets
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

/* Audio codec IDs (match RAD/RSD) */
#define CODEC_PCMU 0
#define CODEC_PCMA 8
#define CODEC_L16  11
#define CODEC_AAC  97
#define CODEC_OPUS 111

/* Minimal valid JPEG: SOI + APP0 + EOI */
static const uint8_t fake_jpeg[] = {
	0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00,
	0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0xFF, 0xD9,
};

/* H.264 NAL units */
static const uint8_t sps[] = {0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xC0,
			      0x1E, 0xD9, 0x00, 0xA0, 0x47, 0xFE, 0xC8};
static const uint8_t pps[] = {0x00, 0x00, 0x00, 0x01, 0x68, 0xCE, 0x38, 0x80};

static int parse_codec(const char *name)
{
	if (!name || strcmp(name, "l16") == 0)
		return CODEC_L16;
	if (strcmp(name, "pcmu") == 0)
		return CODEC_PCMU;
	if (strcmp(name, "pcma") == 0)
		return CODEC_PCMA;
	if (strcmp(name, "aac") == 0)
		return CODEC_AAC;
	if (strcmp(name, "opus") == 0)
		return CODEC_OPUS;
	fprintf(stderr, "Unknown codec: %s (use: l16 pcmu pcma aac opus)\n", name);
	return -1;
}

static int audio_sample_rate(int codec)
{
	if (codec == CODEC_OPUS)
		return 48000;
	return 16000;
}

/* Generate a fake audio frame for the given codec.
 * Returns frame size in bytes. */
static int make_audio_frame(uint8_t *buf, int buf_size, int codec, uint32_t frame_num)
{
	(void)frame_num;

	switch (codec) {
	case CODEC_PCMU:
	case CODEC_PCMA:
		/* 20ms at 16kHz = 320 samples, 1 byte each for G.711 */
		if (buf_size < 320)
			return 0;
		memset(buf, 0x7F, 320); /* silence in mu-law/A-law */
		return 320;

	case CODEC_L16:
		/* 20ms at 16kHz = 320 samples, 2 bytes each (network byte order) */
		if (buf_size < 640)
			return 0;
		memset(buf, 0x00, 640); /* silence */
		return 640;

	case CODEC_AAC: {
		/* Fake raw AAC frame (no ADTS — matches RAD's RAW_STREAM output).
		 * Real AAC frames start with various bit patterns; use recognizable
		 * but non-zero data so we can verify the ADTS wrapper works. */
		int frame_len = 128; /* typical AAC frame at low bitrate */
		if (buf_size < frame_len)
			return 0;
		memset(buf, 0xAA, frame_len);
		buf[0] = 0x01; /* fake AAC data (not silence) */
		return frame_len;
	}

	case CODEC_OPUS: {
		/* Fake Opus packet. Real Opus packets have a TOC byte:
		 * bits 7-3 = config, bit 2 = stereo, bits 1-0 = frame count.
		 * 0xFC = config 31 (48kHz, 20ms), mono, 1 frame */
		int pkt_len = 80; /* typical Opus packet */
		if (buf_size < pkt_len)
			return 0;
		memset(buf, 0x00, pkt_len);
		buf[0] = 0xFC; /* TOC: 20ms, mono, 1 frame */
		return pkt_len;
	}

	default:
		return 0;
	}
}

int main(int argc, char **argv)
{
	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);

	rss_log_init("create_rings", RSS_LOG_INFO, RSS_LOG_TARGET_STDERR, NULL);

	int audio_codec = parse_codec(argc > 1 ? argv[1] : NULL);
	if (audio_codec < 0)
		return 1;

	int sample_rate = audio_sample_rate(audio_codec);

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
		rss_ring_set_stream_info(audio, 64, audio_codec, 0, 0, sample_rate, 1, 0, 0);

	/* Create speaker ring (for RAD output / backchannel) */
	rss_ring_t *speaker = rss_ring_create("speaker", 32, 128 * 1024);
	if (speaker)
		rss_ring_set_stream_info(speaker, 65, CODEC_L16, 0, 0, 16000, 1, 0, 0);

	RSS_INFO("created rings: main sub jpeg0 jpeg1 audio(codec=%d rate=%d) speaker",
		 audio_codec, sample_rate);

	/* Publish initial JPEG frames so RHD has data immediately */
	rss_iov_t iov = {.data = fake_jpeg, .length = sizeof(fake_jpeg)};
	if (jpeg0)
		for (int i = 0; i < 4; i++)
			rss_ring_publish_iov(jpeg0, &iov, 1, i * 40000, 0x30, 1);
	if (jpeg1)
		for (int i = 0; i < 4; i++)
			rss_ring_publish_iov(jpeg1, &iov, 1, i * 40000, 0x30, 1);

	/* Build IDR frame: [SPS][PPS][IDR_slice] */
	uint8_t idr_frame[4096];
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

	/* Build P-frame: [P_slice] */
	uint8_t p_frame[512];
	p_frame[0] = 0x00;
	p_frame[1] = 0x00;
	p_frame[2] = 0x00;
	p_frame[3] = 0x01;
	p_frame[4] = 0x41; /* P-slice */
	memset(p_frame + 5, 0xCD, sizeof(p_frame) - 5);

	/* Audio frame buffer */
	uint8_t audio_frame[4096];

	int64_t vts = 0;  /* video timestamp (µs) */
	int64_t ats = 0;  /* audio timestamp (µs) */
	uint32_t frame_num = 0;

	RSS_INFO("publishing fake H.264+audio at 25fps (audio codec=%d)...", audio_codec);

	while (g_running) {
		bool is_key = (frame_num % 50 == 0);
		const uint8_t *data = is_key ? idr_frame : p_frame;
		uint32_t len = is_key ? (uint32_t)sizeof(idr_frame) : (uint32_t)sizeof(p_frame);
		uint16_t nal = is_key ? 0x13 : 0x14;

		/* Publish to main ring */
		rss_iov_t viov = {.data = data, .length = len};
		if (main_ring)
			rss_ring_publish_iov(main_ring, &viov, 1, vts, nal, is_key ? 1 : 0);

		/* Publish to sub ring (same data, different ring) */
		if (sub_ring)
			rss_ring_publish_iov(sub_ring, &viov, 1, vts, nal, is_key ? 1 : 0);

		/* Publish JPEG snapshot at 1fps */
		if (frame_num % 25 == 0) {
			rss_iov_t jiov = {.data = fake_jpeg, .length = sizeof(fake_jpeg)};
			if (jpeg0)
				rss_ring_publish_iov(jpeg0, &jiov, 1, vts, 0x30, 1);
			if (jpeg1)
				rss_ring_publish_iov(jpeg1, &jiov, 1, vts, 0x30, 1);
		}

		/* 2 audio frames per video frame (20ms audio × 2 = 40ms = 1 video frame) */
		for (int a = 0; a < 2; a++) {
			int alen = make_audio_frame(audio_frame, sizeof(audio_frame),
						    audio_codec, frame_num * 2 + a);
			if (alen > 0 && audio) {
				rss_iov_t aiov = {.data = audio_frame, .length = (uint32_t)alen};
				rss_ring_publish_iov(audio, &aiov, 1, ats, audio_codec, 0);
			}
			ats += 20000; /* 20ms in µs */
		}

		vts += 40000; /* 40ms = 25fps */
		frame_num++;

		usleep(40000);
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
	if (speaker)
		rss_ring_destroy(speaker);

	RSS_INFO("rings destroyed");
	return 0;
}

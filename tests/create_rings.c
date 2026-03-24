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
	rss_ring_t *jpeg0 = rss_ring_create("jpeg0", 16, 2 * 1024 * 1024);
	if (jpeg0)
		rss_ring_set_stream_info(jpeg0, 32, 2, 1920, 1080, 1, 1, 0, 0);

	rss_ring_t *jpeg1 = rss_ring_create("jpeg1", 16, 512 * 1024);
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

	RSS_INFO("published fake JPEG frames, waiting... (Ctrl-C to stop)");

	while (g_running)
		sleep(1);

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

/*
 * ringdump.c -- SHM ring buffer inspection tool
 *
 * Usage:
 *   ringdump <ring>           Print ring header and exit
 *   ringdump <ring> -f        Follow mode: print per-frame metadata
 *   ringdump <ring> -d        Dump raw Annex B to stdout (pipe to ffprobe)
 *   ringdump <ring> -f -n 10  Follow mode, stop after 10 frames
 *
 * Ring names: main, sub, audio
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <inttypes.h>
#include <getopt.h>
#include <stdatomic.h>

#include <rss_ipc.h>
#include <rss_common.h>

static volatile sig_atomic_t g_running = 1;

static void sighandler(int sig)
{
	(void)sig;
	g_running = 0;
}

static const char *nal_type_str(uint16_t nal)
{
	switch (nal) {
	case 0x10:
		return "H264_SPS";
	case 0x11:
		return "H264_PPS";
	case 0x12:
		return "H264_SEI";
	case 0x13:
		return "H264_IDR";
	case 0x14:
		return "H264_SLICE";
	case 0x20:
		return "H265_VPS";
	case 0x21:
		return "H265_SPS";
	case 0x22:
		return "H265_PPS";
	case 0x23:
		return "H265_SEI";
	case 0x24:
		return "H265_IDR";
	case 0x25:
		return "H265_SLICE";
	case 0x30:
		return "JPEG";
	default:
		return "UNKNOWN";
	}
}

static const char *codec_str(uint32_t codec)
{
	switch (codec) {
	case 0:
		return "H.264";
	case 1:
		return "H.265";
	case 2:
		return "JPEG";
	case 3:
		return "MJPEG";
	default:
		return "unknown";
	}
}

static void print_header(const rss_ring_header_t *hdr, const char *name)
{
	fprintf(stderr,
		"Ring: %s\n"
		"  Magic:     0x%08x (%s)\n"
		"  Version:   %u\n"
		"  Stream:    %u\n"
		"  Codec:     %s (%u)\n"
		"  Size:      %ux%u\n"
		"  FPS:       %u/%u\n"
		"  Slots:     %u\n"
		"  Data:      %u bytes (%.1f MB)\n"
		"  Write seq: %" PRIu64 "\n",
		name, hdr->magic, (hdr->magic == RSS_RING_MAGIC) ? "OK" : "BAD", hdr->version,
		hdr->stream_id, codec_str(hdr->codec), hdr->codec, hdr->width, hdr->height,
		hdr->fps_num, hdr->fps_den, hdr->slot_count, hdr->data_size,
		(double)hdr->data_size / (1024.0 * 1024.0), atomic_load(&hdr->write_seq));
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s <ring_name> [options]\n"
		"  -f          Follow mode (print each frame)\n"
		"  -d          Dump raw frame data to stdout\n"
		"  -n <count>  Stop after <count> frames\n"
		"  -h          Show this help\n"
		"\n"
		"Ring names: main, sub, audio\n"
		"\n"
		"Examples:\n"
		"  %s main              Show ring header\n"
		"  %s main -f           Follow frames\n"
		"  %s main -d | ffprobe -i -   Analyze stream\n",
		prog, prog, prog, prog);
}

int main(int argc, char **argv)
{
	if (argc < 2 || strcmp(argv[1], "-h") == 0) {
		usage(argv[0]);
		return (argc < 2) ? 1 : 0;
	}

	const char *ring_name = argv[1];
	bool follow = false;
	bool dump_raw = false;
	int max_frames = 0;

	int opt;
	optind = 2; /* skip ring name */
	while ((opt = getopt(argc, argv, "fdn:h")) != -1) {
		switch (opt) {
		case 'f':
			follow = true;
			break;
		case 'd':
			dump_raw = true;
			break;
		case 'n':
			max_frames = atoi(optarg);
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);

	rss_ring_t *ring = rss_ring_open(ring_name);
	if (!ring) {
		fprintf(stderr, "Cannot open ring '%s' -- not created yet?\n", ring_name);
		return 1;
	}

	const rss_ring_header_t *hdr = rss_ring_get_header(ring);
	print_header(hdr, ring_name);

	if (!follow && !dump_raw) {
		rss_ring_close(ring);
		return 0;
	}

	fprintf(stderr, "\n--- %s ---\n", follow ? "following frames" : "dumping raw data");

	uint64_t read_seq = 0;
	uint64_t frame_count = 0;
	int64_t first_ts = 0;
	int64_t last_ts = 0;
	uint64_t total_bytes = 0;

	/* Allocate read buffer based on ring slot capacity */
	const rss_ring_header_t *rhdr = rss_ring_get_header(ring);
	uint32_t buf_size = rhdr->data_size / rhdr->slot_count;
	if (buf_size < 256 * 1024)
		buf_size = 256 * 1024;
	uint8_t *frame_buf = malloc(buf_size);
	if (!frame_buf) {
		fprintf(stderr, "Failed to allocate %u byte read buffer\n", buf_size);
		rss_ring_close(ring);
		return 1;
	}

	while (g_running) {
		int ret = rss_ring_wait(ring, 1000);
		if (ret != 0)
			continue;

		uint32_t length;
		rss_ring_slot_t meta;

		ret = rss_ring_read(ring, &read_seq, frame_buf, buf_size, &length, &meta);
		if (ret == RSS_EOVERFLOW) {
			fprintf(stderr, "[OVERFLOW] consumer fell behind\n");
			continue;
		}
		if (ret != 0)
			continue;

		if (frame_count == 0)
			first_ts = meta.timestamp;

		int64_t dt = meta.timestamp - last_ts;
		last_ts = meta.timestamp;
		total_bytes += length;

		if (dump_raw) {
			fwrite(frame_buf, 1, length, stdout);
			fflush(stdout);
		} else {
			fprintf(stderr,
				"#%-6" PRIu64 " seq=%-8" PRIu64 " len=%-8u dt=%-8" PRId64 "us "
				"nal=%-12s key=%u\n",
				frame_count, meta.seq, length, dt, nal_type_str(meta.nal_type),
				meta.is_key);
		}

		frame_count++;
		if (max_frames > 0 && (int)frame_count >= max_frames)
			break;
	}

	/* Summary */
	int64_t duration = last_ts - first_ts;
	double dur_sec = duration > 0 ? (double)duration / 1000000.0 : 0;
	double avg_fps = dur_sec > 0 ? (double)frame_count / dur_sec : 0;
	double avg_bps = dur_sec > 0 ? (double)total_bytes * 8.0 / dur_sec : 0;

	fprintf(stderr,
		"\n--- summary ---\n"
		"Frames:   %" PRIu64 "\n"
		"Duration: %.1f s\n"
		"Avg FPS:  %.1f\n"
		"Avg rate: %.0f bps (%.1f kbps)\n"
		"Total:    %" PRIu64 " bytes (%.1f MB)\n",
		frame_count, dur_sec, avg_fps, avg_bps, avg_bps / 1000.0, total_bytes,
		(double)total_bytes / (1024.0 * 1024.0));

	free(frame_buf);
	rss_ring_close(ring);
	return 0;
}

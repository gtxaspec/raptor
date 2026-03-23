/*
 * rvd_frame_loop.c -- Per-channel encoder threads
 *
 * Each encoder channel runs in a dedicated thread, as recommended by
 * the Ingenic SDK documentation (T31 Bitrate Control API Reference,
 * Section 10.3b). This prevents one channel's enc_poll from starving
 * the other and eliminates frame drops on dual-stream setups.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/epoll.h>

#include "rvd.h"

/*
 * Concatenate all NAL units from a frame into the scratch buffer.
 *
 * The HAL returns NAL data that already includes Annex B start codes
 * (00 00 00 01), so we simply concatenate the packs as-is.
 *
 * Returns total length, or 0 on overflow.
 */
static uint32_t linearize_frame(const rss_frame_t *frame,
				uint8_t *buf, uint32_t buf_size)
{
	uint32_t off = 0;

	for (uint32_t i = 0; i < frame->nal_count; i++) {
		uint32_t needed = frame->nals[i].length;
		if (off + needed > buf_size)
			return 0;  /* overflow */

		memcpy(buf + off, frame->nals[i].data, frame->nals[i].length);
		off += frame->nals[i].length;
	}

	return off;
}

/*
 * Determine the primary NAL type for ring metadata.
 */
static uint16_t primary_nal_type(const rss_frame_t *frame)
{
	for (uint32_t i = frame->nal_count; i > 0; i--) {
		rss_nal_type_t t = frame->nals[i - 1].type;
		if (t == RSS_NAL_H264_IDR || t == RSS_NAL_H264_SLICE ||
		    t == RSS_NAL_H265_IDR || t == RSS_NAL_H265_SLICE ||
		    t == RSS_NAL_JPEG_FRAME)
			return (uint16_t)t;
	}
	return frame->nal_count > 0 ? (uint16_t)frame->nals[0].type
				    : (uint16_t)RSS_NAL_UNKNOWN;
}

/* ── Per-channel encoder thread ── */

typedef struct {
	rvd_state_t *st;
	int          idx;
} enc_thread_arg_t;

static void *encoder_thread(void *arg)
{
	enc_thread_arg_t *a = arg;
	rvd_state_t *st = a->st;
	int idx = a->idx;
	rvd_stream_t *s = &st->streams[idx];

	RSS_INFO("encoder thread[%d] started (chn=%d %ux%u)",
		 idx, s->chn, s->enc_cfg.width, s->enc_cfg.height);

	uint64_t frame_count = 0;
	int64_t last_stats = rss_timestamp_us();

	while (*st->running) {
		/* Block until encoder has a frame (up to 1 second timeout).
		 * Each thread blocks independently so channels don't starve. */
		int ret = RSS_HAL_CALL(st->ops, enc_poll, st->hal_ctx,
				       s->chn, 1000);
		if (ret != RSS_OK)
			continue;

		rss_frame_t frame;
		ret = RSS_HAL_CALL(st->ops, enc_get_frame, st->hal_ctx,
				   s->chn, &frame);
		if (ret != RSS_OK)
			continue;

		/* Linearize NALs into per-stream scratch buffer */
		uint32_t len = linearize_frame(&frame, s->scratch,
					       RVD_SCRATCH_SIZE);
		if (len == 0) {
			RSS_WARN("stream%d: frame too large for scratch", idx);
			goto release;
		}

		/* Publish to ring — immediately release the SDK buffer */
		rss_ring_publish(s->ring, s->scratch, len,
				 frame.timestamp,
				 primary_nal_type(&frame),
				 frame.is_key ? 1 : 0);

release:
		RSS_HAL_CALL(st->ops, enc_release_frame, st->hal_ctx,
			     s->chn, &frame);

		frame_count++;

		int64_t now = rss_timestamp_us();
		if (now - last_stats >= 30000000) {
			RSS_INFO("stream%d: %llu frames",
				 idx, (unsigned long long)frame_count);
			last_stats = now;
		}
	}

	RSS_INFO("encoder thread[%d] exiting", idx);
	return NULL;
}

/* ── Control socket handler ── */

static int rvd_ctrl_handler(const char *cmd_json, char *resp_buf,
			    int resp_buf_size, void *userdata)
{
	rvd_state_t *st = userdata;

	if (strstr(cmd_json, "\"request-idr\"")) {
		for (int i = 0; i < st->stream_count; i++)
			RSS_HAL_CALL(st->ops, enc_request_idr, st->hal_ctx,
				     st->streams[i].chn);
		snprintf(resp_buf, resp_buf_size, "{\"status\":\"ok\"}");
		return 0;
	}

	if (strstr(cmd_json, "\"status\"")) {
		snprintf(resp_buf, resp_buf_size,
			 "{\"status\":\"ok\",\"streams\":%d}",
			 st->stream_count);
		return 0;
	}

	snprintf(resp_buf, resp_buf_size,
		 "{\"status\":\"error\",\"reason\":\"unknown command\"}");
	return 0;
}

/* ── Main frame loop: launches threads, handles control socket ── */

void rvd_frame_loop(rvd_state_t *st, volatile sig_atomic_t *running)
{
	st->running = running;

	/* Allocate per-stream scratch buffers */
	for (int i = 0; i < st->stream_count; i++) {
		st->streams[i].scratch = malloc(RVD_SCRATCH_SIZE);
		if (!st->streams[i].scratch) {
			RSS_FATAL("failed to allocate scratch for stream %d", i);
			return;
		}
	}

	/* Start per-channel encoder threads */
	pthread_t enc_tids[RVD_MAX_STREAMS];
	enc_thread_arg_t enc_args[RVD_MAX_STREAMS];

	for (int i = 0; i < st->stream_count; i++) {
		enc_args[i] = (enc_thread_arg_t){ .st = st, .idx = i };
		pthread_create(&enc_tids[i], NULL, encoder_thread, &enc_args[i]);
	}

	/* Main thread: handle control socket + OSD */
	int epoll_fd = epoll_create1(0);
	int ctrl_fd = -1;

	if (st->ctrl && epoll_fd >= 0) {
		ctrl_fd = rss_ctrl_get_fd(st->ctrl);
		if (ctrl_fd >= 0) {
			struct epoll_event ev = {
				.events = EPOLLIN,
				.data.fd = ctrl_fd
			};
			epoll_ctl(epoll_fd, EPOLL_CTL_ADD, ctrl_fd, &ev);
		}
	}

	while (*running) {
		/* Check OSD updates */
		rvd_osd_check(st);

		/* Check control socket */
		if (epoll_fd >= 0) {
			struct epoll_event events[4];
			int n = epoll_wait(epoll_fd, events, 4, 100);
			for (int i = 0; i < n; i++) {
				if (events[i].data.fd == ctrl_fd)
					rss_ctrl_accept_and_handle(st->ctrl,
						rvd_ctrl_handler, st);
			}
		} else {
			usleep(100000);
		}
	}

	/* Wait for encoder threads */
	for (int i = 0; i < st->stream_count; i++)
		pthread_join(enc_tids[i], NULL);

	if (epoll_fd >= 0)
		close(epoll_fd);

	/* Free scratch buffers */
	for (int i = 0; i < st->stream_count; i++) {
		free(st->streams[i].scratch);
		st->streams[i].scratch = NULL;
	}
}

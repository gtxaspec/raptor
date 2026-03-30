/*
 * rvd_frame_loop.c -- Per-channel encoder threads
 *
 * Each encoder channel runs in a dedicated thread, as recommended by
 * the Ingenic SDK documentation (T31 Bitrate Control API Reference,
 * Section 10.3b). This prevents one channel's enc_poll from starving
 * the other and eliminates frame drops on dual-stream setups.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/epoll.h>

#include "rvd.h"

/*
 * Determine the primary NAL type for ring metadata.
 */
static uint16_t primary_nal_type(const rss_frame_t *frame)
{
	for (uint32_t i = frame->nal_count; i > 0; i--) {
		rss_nal_type_t t = frame->nals[i - 1].type;
		if (t == RSS_NAL_H264_IDR || t == RSS_NAL_H264_SLICE || t == RSS_NAL_H265_IDR ||
		    t == RSS_NAL_H265_SLICE || t == RSS_NAL_JPEG_FRAME)
			return (uint16_t)t;
	}
	return frame->nal_count > 0 ? (uint16_t)frame->nals[0].type : (uint16_t)RSS_NAL_UNKNOWN;
}

/* ── Per-channel encoder thread ── */

typedef struct {
	rvd_state_t *st;
	int idx;
} enc_thread_arg_t;

static void *encoder_thread(void *arg)
{
	enc_thread_arg_t *a = arg;
	rvd_state_t *st = a->st;
	int idx = a->idx;
	rvd_stream_t *s = &st->streams[idx];

	RSS_INFO("encoder thread[%d] started (chn=%d %ux%u)", idx, s->chn, s->enc_cfg.width,
		 s->enc_cfg.height);

	uint64_t frame_count = 0;
	int64_t last_stats = rss_timestamp_us();

	while (*st->running) {
		/* Check for consumer IDR request (set via ring header flag) */
		if (s->ring && rss_ring_check_idr(s->ring))
			RSS_HAL_CALL(st->ops, enc_request_idr, st->hal_ctx, s->chn);

		/* Block until encoder has a frame (up to 1 second timeout).
		 * Each thread blocks independently so channels don't starve. */
		int ret = RSS_HAL_CALL(st->ops, enc_poll, st->hal_ctx, s->chn, 1000);
		if (ret != RSS_OK)
			continue;

		rss_frame_t frame;
		ret = RSS_HAL_CALL(st->ops, enc_get_frame, st->hal_ctx, s->chn, &frame);
		if (ret != RSS_OK)
			continue;

		/* Publish NALs directly to ring via scatter-gather */
		rss_iov_t iov[16];
		uint32_t cnt = frame.nal_count;
		if (cnt > 16)
			cnt = 16;
		for (uint32_t n = 0; n < cnt; n++) {
			iov[n].data = frame.nals[n].data;
			iov[n].length = frame.nals[n].length;
		}
		rss_ring_publish_iov(s->ring, iov, cnt, frame.timestamp, primary_nal_type(&frame),
				     frame.is_key ? 1 : 0);

		RSS_HAL_CALL(st->ops, enc_release_frame, st->hal_ctx, s->chn, &frame);

		frame_count++;

		int64_t now = rss_timestamp_us();
		if (now - last_stats >= 30000000) {
			RSS_INFO("stream%d: %llu frames", idx, (unsigned long long)frame_count);
			last_stats = now;
		}
	}

	RSS_INFO("encoder thread[%d] exiting", idx);
	return NULL;
}

/* ── Main frame loop: launches threads, handles control socket ── */

void rvd_frame_loop(rvd_state_t *st, volatile sig_atomic_t *running)
{
	st->running = running;

	/* Start per-channel encoder threads */
	pthread_t enc_tids[RVD_MAX_STREAMS];
	enc_thread_arg_t enc_args[RVD_MAX_STREAMS];

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 128 * 1024);

	for (int i = 0; i < st->stream_count; i++) {
		enc_args[i] = (enc_thread_arg_t){.st = st, .idx = i};
		pthread_create(&enc_tids[i], &attr, encoder_thread, &enc_args[i]);
	}
	pthread_attr_destroy(&attr);

	/* OSD update thread — runs HAL OSD calls in isolation to avoid
	 * interfering with the encoder/ring path. IMP_OSD_UpdateRgnAttrData
	 * shares internal SDK state with the encoder and can block the
	 * futex wake that ring consumers depend on. */
	pthread_t osd_tid = 0;
	if (st->osd_enabled) {
		pthread_attr_t osd_attr;
		pthread_attr_init(&osd_attr);
		pthread_attr_setstacksize(&osd_attr, 128 * 1024);
		pthread_create(&osd_tid, &osd_attr, rvd_osd_thread, st);
		pthread_attr_destroy(&osd_attr);
	}

	/* IVS poll thread */
	pthread_t ivs_tid = 0;
	if (st->ivs_active) {
		pthread_attr_t ivs_attr;
		pthread_attr_init(&ivs_attr);
		pthread_attr_setstacksize(&ivs_attr, 64 * 1024);
		pthread_create(&ivs_tid, &ivs_attr, rvd_ivs_thread, st);
		pthread_attr_destroy(&ivs_attr);
	}

	/* Main thread: handle control socket */
	int epoll_fd = epoll_create1(0);
	int ctrl_fd = -1;

	if (st->ctrl && epoll_fd >= 0) {
		ctrl_fd = rss_ctrl_get_fd(st->ctrl);
		if (ctrl_fd >= 0) {
			struct epoll_event ev = {.events = EPOLLIN, .data.fd = ctrl_fd};
			if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, ctrl_fd, &ev) < 0)
				RSS_ERROR("epoll_ctl add ctrl_fd: %s", strerror(errno));
		}
	}

	while (*running) {
		/* Check control socket */
		if (epoll_fd >= 0) {
			struct epoll_event events[4];
			int n = epoll_wait(epoll_fd, events, 4, 100);
			for (int i = 0; i < n; i++) {
				if (events[i].data.fd == ctrl_fd)
					rss_ctrl_accept_and_handle(st->ctrl, rvd_ctrl_handler, st);
			}
		} else {
			usleep(100000);
		}
	}

	/* Wait for encoder threads */
	for (int i = 0; i < st->stream_count; i++)
		pthread_join(enc_tids[i], NULL);

	if (osd_tid)
		pthread_join(osd_tid, NULL);

	if (ivs_tid)
		pthread_join(ivs_tid, NULL);

	if (epoll_fd >= 0)
		close(epoll_fd);
}

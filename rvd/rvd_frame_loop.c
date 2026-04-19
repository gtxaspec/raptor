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

void *rvd_encoder_thread(void *arg)
{
	rvd_enc_thread_arg_t *a = arg;
	rvd_state_t *st = a->st;
	int idx = a->idx;
	rvd_stream_t *s = &st->streams[idx];

	RSS_DEBUG("encoder thread[%d] started (chn=%d %ux%u)", idx, s->chn, s->enc_cfg.width,
		  s->enc_cfg.height);

	uint64_t frame_count = 0;
	int poll_errors = 0;
	int64_t last_stats = rss_timestamp_us();
	int64_t last_reap = last_stats;

	while (*st->running && atomic_load(&st->stream_active[idx])) {
		/* JPEG on-demand: start/stop encoder based on ring consumers */
		if (s->is_jpeg && s->jpeg_idle && s->ring) {
			/* Periodically check for crashed consumers (~10s) */
			int64_t now_reap = rss_timestamp_us();
			if (now_reap - last_reap >= 10000000) {
				RSS_TRACE("jpeg chn %d: reap check (readers=%u pids=[%u,%u,%u,%u])",
					  s->chn, rss_ring_reader_count(s->ring),
					  rss_ring_get_header(s->ring)->reader_pids[0],
					  rss_ring_get_header(s->ring)->reader_pids[1],
					  rss_ring_get_header(s->ring)->reader_pids[2],
					  rss_ring_get_header(s->ring)->reader_pids[3]);
				uint32_t reaped = rss_ring_reap_dead_readers(s->ring);
				if (reaped)
					RSS_WARN("jpeg chn %d: reaped %u dead reader(s)", s->chn,
						 reaped);
				last_reap = now_reap;
			}

			if (rss_ring_reader_count(s->ring) == 0) {
				if (s->enabled) {
					RSS_HAL_CALL(st->ops, enc_stop, st->hal_ctx, s->chn);
					s->enabled = false;
					RSS_INFO("jpeg chn %d: stopped (no consumers)", s->chn);
				}
				usleep(100000);
				continue;
			}
			if (!s->enabled) {
				RSS_HAL_CALL(st->ops, enc_start, st->hal_ctx, s->chn);
				s->enabled = true;
				RSS_INFO("jpeg chn %d: started (%u consumers)", s->chn,
					 rss_ring_reader_count(s->ring));
			}
		}

		/* Check for consumer IDR request (set via ring header flag) */
		if (s->ring && rss_ring_check_idr(s->ring))
			RSS_HAL_CALL(st->ops, enc_request_idr, st->hal_ctx, s->chn);

		/* Block until encoder has a frame (up to 1 second timeout).
		 * Each thread blocks independently so channels don't starve. */
		int ret = RSS_HAL_CALL(st->ops, enc_poll, st->hal_ctx, s->chn, 1000);
		if (ret != RSS_OK) {
			/* Timeouts are normal (sensor idle, JPEG on-demand stopped).
			 * Log on repeated failures to catch flaky sensor/encoder. */
			if (++poll_errors == 10)
				RSS_WARN("stream%d: enc_poll failing (chn %d, last=%d)", idx,
					 s->chn, ret);
			continue;
		}
		poll_errors = 0;

		rss_frame_t frame;
		ret = RSS_HAL_CALL(st->ops, enc_get_frame, st->hal_ctx, s->chn, &frame);
		if (ret != RSS_OK) {
			RSS_WARN("stream%d: enc_get_frame failed (chn %d, ret=%d)", idx, s->chn,
				 ret);
			continue;
		}

		if (st->refmode && st->rmem_virt_base && frame.nal_count > 0) {
			uintptr_t vaddr = (uintptr_t)frame.nals[0].data;
			uint32_t rmem_off = (uint32_t)(vaddr - st->rmem_virt_base);
			uint32_t total_len = 0;
			for (uint32_t n = 0; n < frame.nal_count; n++)
				total_len += frame.nals[n].length;

			/* Track encoder buffer region base from first frame */
			if (!s->enc_buf_base)
				s->enc_buf_base = vaddr;
			uint8_t buf_idx = (uint8_t)((vaddr - s->enc_buf_base) /
						    s->enc_cfg.stream_buf_size);

			rss_ring_publish_ref(s->ring, rmem_off, total_len, frame.timestamp,
					     primary_nal_type(&frame), frame.is_key ? 1 : 0,
					     buf_idx);
		} else {
			/* Embedded mode: copy NALs into ring via scatter-gather */
			rss_iov_t iov[16];
			uint32_t cnt = frame.nal_count;
			if (cnt > 16)
				cnt = 16;
			for (uint32_t n = 0; n < cnt; n++) {
				iov[n].data = frame.nals[n].data;
				iov[n].length = frame.nals[n].length;
			}
			rss_ring_publish_iov(s->ring, iov, cnt, frame.timestamp,
					     primary_nal_type(&frame), frame.is_key ? 1 : 0);
		}

		RSS_HAL_CALL(st->ops, enc_release_frame, st->hal_ctx, s->chn, &frame);

		frame_count++;

		int64_t now = rss_timestamp_us();
		if (now - last_stats >= 30000000) {
			RSS_DEBUG("stream%d: %llu frames", idx, (unsigned long long)frame_count);
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
	for (int i = 0; i < st->stream_count; i++)
		rvd_stream_start(st, i);

	/* OSD update thread */
	pthread_t osd_tid = 0;
	if (st->osd_enabled) {
		pthread_attr_t osd_attr;
		pthread_attr_init(&osd_attr);
		pthread_attr_setstacksize(&osd_attr, 128 * 1024);
		pthread_create(&osd_tid, &osd_attr, rvd_osd_thread, st);
		pthread_attr_destroy(&osd_attr);
	}

	/* IVS poll thread */
	if (st->ivs_active) {
		pthread_attr_t ivs_attr;
		pthread_attr_init(&ivs_attr);
		pthread_attr_setstacksize(&ivs_attr, 64 * 1024);
		pthread_create(&st->ivs_tid, &ivs_attr, rvd_ivs_thread, st);
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

	/* Stop all encoder threads + disable FS + stop enc */
	for (int i = 0; i < st->stream_count; i++)
		rvd_stream_stop(st, i);

	if (osd_tid)
		pthread_join(osd_tid, NULL);

	if (st->ivs_tid) {
		pthread_join(st->ivs_tid, NULL);
		st->ivs_tid = 0;
	}

	if (epoll_fd >= 0)
		close(epoll_fd);
}

/*
 * rvd_frame_loop.c -- Encoder polling and ring buffer publishing
 *
 * The hot loop: polls the hardware encoder for completed frames,
 * linearizes NAL units with Annex B start codes, and publishes
 * each frame to the SHM ring buffer.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
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
 * For keyframes this is the IDR NAL; for non-key frames it's the slice.
 * We look past SPS/PPS/VPS/SEI to find the actual picture NAL.
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
	/* Fallback: first NAL type */
	return frame->nal_count > 0 ? (uint16_t)frame->nals[0].type
				    : (uint16_t)RSS_NAL_UNKNOWN;
}

/* Process one encoder channel: poll, get frame, publish to ring */
static void process_channel(rvd_state_t *st, int idx, uint32_t timeout_ms)
{
	rvd_stream_t *s = &st->streams[idx];
	if (!s->enabled || !s->ring) return;

	int ret = RSS_HAL_CALL(st->ops, enc_poll, st->hal_ctx, s->chn, timeout_ms);
	if (ret != RSS_OK) {
		RSS_DEBUG("stream%d: enc_poll returned %d", idx, ret);
		return;
	}

	rss_frame_t frame;
	ret = RSS_HAL_CALL(st->ops, enc_get_frame, st->hal_ctx, s->chn, &frame);
	if (ret != RSS_OK) return;

	/* Linearize NALs into scratch buffer */
	uint32_t len = linearize_frame(&frame, st->scratch, RVD_SCRATCH_SIZE);
	if (len == 0) {
		RSS_WARN("stream%d: frame too large for scratch buffer", idx);
		goto release;
	}

	/* Publish to ring */
	rss_ring_publish(s->ring, st->scratch, len,
			 frame.timestamp,
			 primary_nal_type(&frame),
			 frame.is_key ? 1 : 0);

release:
	RSS_HAL_CALL(st->ops, enc_release_frame, st->hal_ctx, s->chn, &frame);
}

/* Control socket handler */
static int rvd_ctrl_handler(const char *cmd_json, char *resp_buf,
			    int resp_buf_size, void *userdata)
{
	rvd_state_t *st = userdata;

	/* Simple command dispatch -- look for "cmd" field */
	if (strstr(cmd_json, "\"request-idr\"")) {
		for (int i = 0; i < st->stream_count; i++)
			RSS_HAL_CALL(st->ops, enc_request_idr, st->hal_ctx,
				     st->streams[i].chn);
		snprintf(resp_buf, resp_buf_size, "{\"status\":\"ok\"}");
		return 0;
	}

	if (strstr(cmd_json, "\"status\"")) {
		const rss_ring_header_t *hdr = NULL;
		if (st->streams[0].ring)
			hdr = rss_ring_get_header(st->streams[0].ring);
		snprintf(resp_buf, resp_buf_size,
			 "{\"status\":\"ok\",\"streams\":%d,\"write_seq\":%llu}",
			 st->stream_count,
			 hdr ? (unsigned long long)__atomic_load_n(
				 &hdr->write_seq, __ATOMIC_RELAXED) : 0ULL);
		return 0;
	}

	snprintf(resp_buf, resp_buf_size,
		 "{\"status\":\"error\",\"reason\":\"unknown command\"}");
	return 0;
}

void rvd_frame_loop(rvd_state_t *st, volatile sig_atomic_t *running)
{
	int epoll_fd = epoll_create1(0);
	if (epoll_fd < 0) {
		RSS_FATAL("epoll_create1 failed");
		return;
	}

	/* Add control socket to epoll (if available) */
	int ctrl_fd = -1;
	if (st->ctrl) {
		ctrl_fd = rss_ctrl_get_fd(st->ctrl);
		if (ctrl_fd >= 0) {
			struct epoll_event ev = {
				.events = EPOLLIN,
				.data.fd = ctrl_fd
			};
			epoll_ctl(epoll_fd, EPOLL_CTL_ADD, ctrl_fd, &ev);
		}
	}

	uint64_t frame_count = 0;
	int64_t last_stats = rss_timestamp_us();

	while (*running) {
		/* Poll all streams. Use short timeout so we service
		 * both channels frequently at 25fps (40ms per frame). */
		for (int i = 0; i < st->stream_count; i++)
			process_channel(st, i, 20);

		/* Check OSD updates */
		rvd_osd_check(st);

		/* Check control socket (non-blocking) */
		if (ctrl_fd >= 0) {
			struct epoll_event events[4];
			int n = epoll_wait(epoll_fd, events, 4, 0);
			for (int i = 0; i < n; i++) {
				if (events[i].data.fd == ctrl_fd)
					rss_ctrl_accept_and_handle(st->ctrl,
						rvd_ctrl_handler, st);
			}
		}

		frame_count++;

		/* Periodic stats */
		int64_t now = rss_timestamp_us();
		if (now - last_stats >= 30000000) {  /* every 30s */
			RSS_INFO("frames processed: %llu",
				 (unsigned long long)frame_count);
			last_stats = now;
		}
	}

	close(epoll_fd);
}

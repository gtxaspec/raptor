/*
 * rwc_main.c — Raptor Webcam Daemon
 *
 * Ring consumer that reads JPEG or H.264 video frames and raw PCM audio
 * from raptor ring buffers and feeds them to the Linux UVC+UAC gadget,
 * making the camera appear as a USB webcam with microphone.
 *
 * Video: V4L2 UVC gadget interface (bulk endpoint)
 * Audio: /dev/uac_mic char device (isochronous endpoint via kernel)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <rss_ipc.h>
#include <rss_common.h>

#include "rwc.h"

/* --------------------------------------------------------------------------
 * State
 */

typedef struct {
	rss_config_t *cfg;
	const char *config_path;
	volatile sig_atomic_t *running;

	/* UVC device */
	int uvc_fd;
	char uvc_device[64];

	/* V4L2 buffers */
	struct rwc_buffer buffers[RWC_MAX_BUFFERS];
	int buf_count;
	bool streaming;

	/* Negotiated format */
	uint8_t cur_format;
	uint8_t cur_frame;
	uint32_t cur_interval;
	struct uvc_streaming_control probe;
	struct uvc_streaming_control commit;
	uint8_t last_cs;

	/* Video rings */
	rss_ring_t *jpeg_ring;
	rss_ring_t *video_ring;
	uint64_t read_seq;
	uint8_t *frame_buf;
	uint32_t frame_buf_size;

	/* Video ring idle tracking */
	uint64_t jpeg_last_ws;
	uint64_t video_last_ws;
	int jpeg_idle;
	int video_idle;

	/* Audio */
	int mic_fd;
	rss_ring_t *audio_ring;
	uint64_t audio_seq;
	uint8_t *audio_buf;
	uint32_t audio_buf_size;
	bool audio_enabled;
	uint64_t audio_last_ws;
	int audio_idle;

	/* Control socket */
	rss_ctrl_t *ctrl;
} rwc_state_t;

/* --------------------------------------------------------------------------
 * Streaming control fill
 *
 * Populate a UVC streaming control struct with clamped values.
 * Used for PROBE/COMMIT GET_CUR/GET_MIN/GET_MAX/GET_DEF responses.
 */

static void fill_streaming_control(struct uvc_streaming_control *ctrl,
                                   uint8_t format_idx, uint8_t frame_idx,
                                   uint32_t interval)
{
	memset(ctrl, 0, sizeof(*ctrl));
	ctrl->bmHint = 1;

	if (format_idx < 1 || format_idx > RWC_NUM_FORMATS)
		format_idx = RWC_FMT_MJPEG;
	ctrl->bFormatIndex = format_idx;

	if (frame_idx < 1 || frame_idx > RWC_NUM_FRAMES)
		frame_idx = RWC_FRAME_1080P;
	ctrl->bFrameIndex = frame_idx;

	if (interval <= RWC_INTERVAL_30FPS)
		interval = RWC_INTERVAL_30FPS;
	else if (interval <= RWC_INTERVAL_25FPS)
		interval = RWC_INTERVAL_25FPS;
	else
		interval = RWC_INTERVAL_15FPS;
	ctrl->dwFrameInterval = interval;

	ctrl->dwMaxVideoFrameSize = rwc_frames[frame_idx].max_size;
	ctrl->dwMaxPayloadTransferSize = 3072;
	ctrl->bmFramingInfo = 3;
	ctrl->bPreferedVersion = 1;
	ctrl->bMinVersion = 1;
	ctrl->bMaxVersion = 1;
}

static int start_streaming(rwc_state_t *st);
static void stop_streaming(rwc_state_t *st);

/* --------------------------------------------------------------------------
 * UVC event handlers
 */

static void handle_setup_event(rwc_state_t *st,
                               const struct usb_ctrlrequest *req)
{
	struct uvc_request_data resp;
	struct uvc_streaming_control sc;

	memset(&resp, 0, sizeof(resp));

	uint8_t type = req->bRequestType;
	uint8_t request = req->bRequest;
	uint8_t cs = req->wValue >> 8;
	uint8_t intf = req->wIndex & 0xff;
	uint16_t wLength = req->wLength;

	/* Stall control interface and unknown streaming requests */
	if (intf != UVC_INTF_STREAMING ||
	    (cs != UVC_VS_PROBE_CONTROL && cs != UVC_VS_COMMIT_CONTROL)) {
		resp.length = -1;
		goto send;
	}

	struct uvc_streaming_control *target =
		(cs == UVC_VS_PROBE_CONTROL) ? &st->probe : &st->commit;

	if (type & USB_DIR_IN) {
		uint16_t len = (wLength < sizeof(sc)) ? wLength : sizeof(sc);

		switch (request) {
		case UVC_GET_CUR:
			memcpy(resp.data, target, len);
			resp.length = len;
			break;
		case UVC_GET_MIN:
			fill_streaming_control(&sc, RWC_FMT_MJPEG,
			                       RWC_FRAME_360P, RWC_INTERVAL_15FPS);
			memcpy(resp.data, &sc, len);
			resp.length = len;
			break;
		case UVC_GET_MAX:
			fill_streaming_control(&sc, RWC_FMT_H264,
			                       RWC_FRAME_1080P, RWC_INTERVAL_30FPS);
			memcpy(resp.data, &sc, len);
			resp.length = len;
			break;
		case UVC_GET_DEF:
			fill_streaming_control(&sc, RWC_FMT_MJPEG,
			                       RWC_FRAME_1080P, RWC_INTERVAL_30FPS);
			memcpy(resp.data, &sc, len);
			resp.length = len;
			break;
		default:
			resp.length = -1;
			break;
		}
	} else {
		st->last_cs = cs;
		resp.length = wLength;
	}

send:
	if (ioctl(st->uvc_fd, UVCIOC_SEND_RESPONSE, &resp) < 0)
		RSS_WARN("UVCIOC_SEND_RESPONSE: %s", strerror(errno));
}

static void handle_data_event(rwc_state_t *st,
                              const struct uvc_request_data *data)
{
	struct uvc_streaming_control proposed;
	struct uvc_streaming_control *target =
		(st->last_cs == UVC_VS_COMMIT_CONTROL) ? &st->commit : &st->probe;

	/* Safe copy from potentially unaligned data */
	memcpy(&proposed, data->data, sizeof(proposed));

	fill_streaming_control(target, proposed.bFormatIndex,
	                       proposed.bFrameIndex, proposed.dwFrameInterval);

	RSS_DEBUG("%s fmt=%u frm=%u int=%u",
	          st->last_cs == UVC_VS_COMMIT_CONTROL ? "COMMIT" : "PROBE",
	          target->bFormatIndex, target->bFrameIndex,
	          target->dwFrameInterval);

	/* Bulk mode: start streaming after COMMIT (no STREAMON event) */
	if (st->last_cs == UVC_VS_COMMIT_CONTROL && !st->streaming) {
		RSS_INFO("COMMIT received, starting streaming");
		if (start_streaming(st) < 0)
			RSS_ERROR("failed to start streaming");
	}
}

/* --------------------------------------------------------------------------
 * V4L2 streaming start / stop
 */

static int start_streaming(rwc_state_t *st)
{
	struct v4l2_requestbuffers rb;
	struct v4l2_format fmt;
	int i;

	/* Apply committed format before allocating buffers */
	st->cur_format = st->probe.bFormatIndex;
	st->cur_frame = st->probe.bFrameIndex;
	st->cur_interval = st->probe.dwFrameInterval;

	/* Set format + imagesize so the kernel allocates properly sized buffers.
	 * For MJPEG, bpp=0 so the kernel uses sizeimage directly.
	 * Use the ring's data_size as a safe upper bound for compressed frames.
	 */
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	fmt.fmt.pix.pixelformat = (st->cur_format == RWC_FMT_H264)
		? V4L2_PIX_FMT_H264 : V4L2_PIX_FMT_MJPEG;
	fmt.fmt.pix.width = rwc_frames[st->cur_frame].width;
	fmt.fmt.pix.height = rwc_frames[st->cur_frame].height;
	fmt.fmt.pix.sizeimage = st->frame_buf_size;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;

	if (ioctl(st->uvc_fd, VIDIOC_S_FMT, &fmt) < 0)
		RSS_WARN("VIDIOC_S_FMT: %s", strerror(errno));
	else
		RSS_DEBUG("V4L2 format: %ux%u sizeimage=%u",
		          fmt.fmt.pix.width, fmt.fmt.pix.height,
		          fmt.fmt.pix.sizeimage);

	memset(&rb, 0, sizeof(rb));
	rb.count = st->buf_count;
	rb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	rb.memory = V4L2_MEMORY_MMAP;

	if (ioctl(st->uvc_fd, VIDIOC_REQBUFS, &rb) < 0) {
		RSS_ERROR("VIDIOC_REQBUFS: %s", strerror(errno));
		return -1;
	}
	st->buf_count = rb.count;

	for (i = 0; i < st->buf_count; i++) {
		struct v4l2_buffer buf;

		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;

		if (ioctl(st->uvc_fd, VIDIOC_QUERYBUF, &buf) < 0) {
			RSS_ERROR("VIDIOC_QUERYBUF %d: %s", i, strerror(errno));
			goto err_unmap;
		}

		st->buffers[i].length = buf.length;
		st->buffers[i].start = mmap(NULL, buf.length,
		                            PROT_READ | PROT_WRITE, MAP_SHARED,
		                            st->uvc_fd, buf.m.offset);
		if (st->buffers[i].start == MAP_FAILED) {
			st->buffers[i].start = NULL;
			RSS_ERROR("mmap buffer %d: %s", i, strerror(errno));
			goto err_unmap;
		}

		/* Queue with bytesused=1 to avoid vb2 zero-bytesused warning.
		 * The actual payload is set when deliver_frame fills the buffer. */
		buf.bytesused = 1;
		if (ioctl(st->uvc_fd, VIDIOC_QBUF, &buf) < 0) {
			RSS_ERROR("VIDIOC_QBUF %d: %s", i, strerror(errno));
			goto err_unmap;
		}
	}

	int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	if (ioctl(st->uvc_fd, VIDIOC_STREAMON, &type) < 0) {
		RSS_ERROR("VIDIOC_STREAMON: %s", strerror(errno));
		goto err_unmap;
	}

	st->read_seq = 0;
	st->streaming = true;

	RSS_INFO("streaming: %s %ux%u @ %u fps",
	         st->cur_format == RWC_FMT_H264 ? "H.264" : "MJPEG",
	         rwc_frames[st->cur_frame].width,
	         rwc_frames[st->cur_frame].height,
	         st->cur_interval ? 10000000 / st->cur_interval : 0);

	return 0;

err_unmap:
	for (int j = 0; j < i; j++) {
		if (st->buffers[j].start) {
			munmap(st->buffers[j].start, st->buffers[j].length);
			st->buffers[j].start = NULL;
		}
	}
	memset(&rb, 0, sizeof(rb));
	rb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	rb.memory = V4L2_MEMORY_MMAP;
	ioctl(st->uvc_fd, VIDIOC_REQBUFS, &rb);
	return -1;
}

static void stop_streaming(rwc_state_t *st)
{
	if (!st->streaming)
		return;

	int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	ioctl(st->uvc_fd, VIDIOC_STREAMOFF, &type);

	for (int i = 0; i < st->buf_count; i++) {
		if (st->buffers[i].start) {
			munmap(st->buffers[i].start, st->buffers[i].length);
			st->buffers[i].start = NULL;
		}
	}

	struct v4l2_requestbuffers rb;
	memset(&rb, 0, sizeof(rb));
	rb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	rb.memory = V4L2_MEMORY_MMAP;
	ioctl(st->uvc_fd, VIDIOC_REQBUFS, &rb);

	st->streaming = false;
	RSS_INFO("streaming stopped");
}

/* --------------------------------------------------------------------------
 * UVC event processing
 */

static void process_events(rwc_state_t *st)
{
	struct v4l2_event ev;

	while (ioctl(st->uvc_fd, VIDIOC_DQEVENT, &ev) == 0) {
		struct uvc_event *uvc_ev = (struct uvc_event *)&ev.u.data;

		switch (ev.type) {
		case UVC_EVENT_CONNECT:
			RSS_INFO("USB connected (speed %d)", uvc_ev->speed);
			break;

		case UVC_EVENT_DISCONNECT:
			RSS_INFO("USB disconnected");
			stop_streaming(st);
			break;

		case UVC_EVENT_SETUP:
			handle_setup_event(st, &uvc_ev->req);
			break;

		case UVC_EVENT_DATA:
			handle_data_event(st, &uvc_ev->data);
			break;

		case UVC_EVENT_STREAMON:
			RSS_INFO("STREAMON");
			if (start_streaming(st) < 0)
				RSS_ERROR("failed to start streaming");
			break;

		case UVC_EVENT_STREAMOFF:
			RSS_INFO("STREAMOFF");
			stop_streaming(st);
			break;

		default:
			RSS_DEBUG("unknown UVC event %u", ev.type);
			break;
		}
	}
}

/* --------------------------------------------------------------------------
 * Video frame delivery
 */

static void deliver_frame(rwc_state_t *st)
{
	rss_ring_t *ring;
	uint32_t length = 0;
	rss_ring_slot_t meta;
	struct v4l2_buffer buf;
	uint32_t copy_len;
	int ret;

	ring = (st->cur_format == RWC_FMT_H264) ? st->video_ring : st->jpeg_ring;
	if (!ring)
		return;

	ret = rss_ring_read(ring, &st->read_seq, st->frame_buf,
	                    st->frame_buf_size, &length, &meta);
	if (ret == RSS_EOVERFLOW && st->read_seq > 0) {
		st->read_seq--;
		ret = rss_ring_read(ring, &st->read_seq, st->frame_buf,
		                    st->frame_buf_size, &length, &meta);
	}
	if (ret != 0 || length == 0)
		return;

	/* Validate JPEG SOI marker */
	if (st->cur_format == RWC_FMT_MJPEG &&
	    (length < 2 || st->frame_buf[0] != 0xFF || st->frame_buf[1] != 0xD8)) {
		RSS_DEBUG("bad JPEG (len=%u hdr=%02x%02x)", length,
		          length > 0 ? st->frame_buf[0] : 0,
		          length > 1 ? st->frame_buf[1] : 0);
		return;
	}

	memset(&buf, 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	buf.memory = V4L2_MEMORY_MMAP;

	if (ioctl(st->uvc_fd, VIDIOC_DQBUF, &buf) < 0) {
		if (errno != EAGAIN)
			RSS_DEBUG("VIDIOC_DQBUF: %s", strerror(errno));
		return;
	}

	if (buf.index >= (unsigned)st->buf_count) {
		RSS_ERROR("DQBUF index %u out of range", buf.index);
		return;
	}

	copy_len = length;
	if (copy_len > st->buffers[buf.index].length) {
		RSS_WARN("frame truncated: %u > %zu", length,
		         st->buffers[buf.index].length);
		copy_len = st->buffers[buf.index].length;
	}

	memcpy(st->buffers[buf.index].start, st->frame_buf, copy_len);
	buf.bytesused = copy_len;

	if (ioctl(st->uvc_fd, VIDIOC_QBUF, &buf) < 0)
		RSS_WARN("VIDIOC_QBUF: %s", strerror(errno));
}

/* --------------------------------------------------------------------------
 * Audio feed — read PCM from audio ring, byte-swap L16, write to /dev/uac_mic
 */

static void feed_audio(rwc_state_t *st)
{
	if (!st->audio_ring || st->mic_fd < 0)
		return;

	for (int i = 0; i < RWC_AUDIO_DRAIN_MAX; i++) {
		uint32_t alen = 0;
		rss_ring_slot_t ameta;

		int ret = rss_ring_read(st->audio_ring, &st->audio_seq,
		                        st->audio_buf, st->audio_buf_size,
		                        &alen, &ameta);
		if (ret == RSS_EOVERFLOW && st->audio_seq > 0) {
			st->audio_seq--;
			ret = rss_ring_read(st->audio_ring, &st->audio_seq,
			                    st->audio_buf, st->audio_buf_size,
			                    &alen, &ameta);
		}
		if (ret != 0 || alen == 0)
			break;

		/* L16 is network byte order (big-endian) — swap to little-endian */
		uint16_t *samples = (uint16_t *)st->audio_buf;
		uint32_t nsamples = alen / 2;
		for (uint32_t s = 0; s < nsamples; s++)
			samples[s] = __builtin_bswap16(samples[s]);

		ssize_t nw = write(st->mic_fd, st->audio_buf, alen);
		if (nw < 0 && errno != EAGAIN)
			RSS_DEBUG("uac_mic write: %s", strerror(errno));
	}
}

/* --------------------------------------------------------------------------
 * Ring management
 */

static rss_ring_t *try_open_ring(const char *name)
{
	rss_ring_t *ring = rss_ring_open(name);
	if (ring) {
		const rss_ring_header_t *hdr = rss_ring_get_header(ring);
		RSS_INFO("ring %s: %ux%u codec=%u fps=%u/%u",
		         name, hdr->width, hdr->height, hdr->codec,
		         hdr->fps_num, hdr->fps_den);
	}
	return ring;
}

/* Check ring for idle (no new writes) and close after threshold */
static void check_ring_idle(rss_ring_t **ring, uint64_t *last_ws,
                            int *idle_count, const char *name)
{
	if (!*ring)
		return;

	const rss_ring_header_t *hdr = rss_ring_get_header(*ring);
	uint64_t ws = hdr->write_seq;

	if (ws == *last_ws)
		(*idle_count)++;
	else
		*idle_count = 0;
	*last_ws = ws;

	if (*idle_count >= 10) {
		RSS_DEBUG("%s ring idle, closing", name);
		rss_ring_close(*ring);
		*ring = NULL;
		*idle_count = 0;
	}
}

/* --------------------------------------------------------------------------
 * UVC device open
 */

static int uvc_open(rwc_state_t *st)
{
	struct v4l2_event_subscription sub;
	static const uint32_t events[] = {
		UVC_EVENT_CONNECT, UVC_EVENT_DISCONNECT,
		UVC_EVENT_STREAMON, UVC_EVENT_STREAMOFF,
		UVC_EVENT_SETUP, UVC_EVENT_DATA,
	};

	st->uvc_fd = open(st->uvc_device, O_RDWR | O_NONBLOCK);
	if (st->uvc_fd < 0) {
		RSS_ERROR("open %s: %s", st->uvc_device, strerror(errno));
		return -1;
	}

	memset(&sub, 0, sizeof(sub));
	for (int i = 0; i < (int)(sizeof(events) / sizeof(events[0])); i++) {
		sub.type = events[i];
		if (ioctl(st->uvc_fd, VIDIOC_SUBSCRIBE_EVENT, &sub) < 0) {
			RSS_ERROR("subscribe event %u: %s", events[i],
			          strerror(errno));
			close(st->uvc_fd);
			st->uvc_fd = -1;
			return -1;
		}
	}

	fill_streaming_control(&st->probe, RWC_FMT_MJPEG, RWC_FRAME_1080P,
	                       RWC_INTERVAL_30FPS);
	st->commit = st->probe;

	RSS_INFO("UVC device %s opened", st->uvc_device);
	return 0;
}

/* --------------------------------------------------------------------------
 * Control socket handler
 */

static int ctrl_handler(const char *cmd_json, char *resp, int resp_size,
                        void *ud)
{
	rwc_state_t *st = ud;

	int rc = rss_ctrl_handle_common(cmd_json, resp, resp_size,
	                                st->cfg, st->config_path);
	if (rc >= 0)
		return rc;

	snprintf(resp, resp_size,
	         "{\"status\":\"ok\",\"streaming\":%s,\"format\":\"%s\","
	         "\"resolution\":\"%ux%u\",\"fps\":%u,\"audio\":%s}",
	         st->streaming ? "true" : "false",
	         st->cur_format == RWC_FMT_H264 ? "h264" : "mjpeg",
	         st->streaming ? rwc_frames[st->cur_frame].width : 0,
	         st->streaming ? rwc_frames[st->cur_frame].height : 0,
	         st->cur_interval ? 10000000 / st->cur_interval : 0,
	         st->audio_enabled ? "true" : "false");
	return (int)strlen(resp);
}

/* --------------------------------------------------------------------------
 * Audio setup
 */

static void audio_init(rwc_state_t *st, const char *ring_name)
{
	/* Check for /dev/uac_mic first — no point waiting for ring if
	 * the kernel UAC function isn't loaded */
	st->mic_fd = open("/dev/uac_mic", O_WRONLY | O_NONBLOCK);
	if (st->mic_fd < 0) {
		RSS_INFO("no /dev/uac_mic (audio disabled)");
		goto disable;
	}

	/* Open audio ring with retry */
	for (int attempt = 0; attempt < 10 && *st->running; attempt++) {
		st->audio_ring = try_open_ring(ring_name);
		if (st->audio_ring)
			break;
		RSS_DEBUG("waiting for audio ring...");
		sleep(1);
	}

	if (!st->audio_ring) {
		RSS_WARN("audio ring not available (audio disabled)");
		goto disable;
	}

	st->audio_buf_size = RWC_AUDIO_BUF_SIZE;
	st->audio_buf = malloc(st->audio_buf_size);
	if (!st->audio_buf) {
		RSS_WARN("audio buffer alloc failed");
		goto disable;
	}

	RSS_INFO("audio: %s → /dev/uac_mic", ring_name);
	return;

disable:
	if (st->mic_fd >= 0) {
		close(st->mic_fd);
		st->mic_fd = -1;
	}
	st->audio_enabled = false;
}

/* --------------------------------------------------------------------------
 * Main
 */

int main(int argc, char **argv)
{
	rss_daemon_ctx_t ctx;
	int ret;

	ret = rss_daemon_init(&ctx, "rwc", argc, argv);
	if (ret != 0)
		return ret < 0 ? 1 : 0;
	RSS_BANNER("rwc");

	if (!rss_config_get_bool(ctx.cfg, "webcam", "enabled", false)) {
		RSS_INFO("webcam disabled in config");
		rss_config_free(ctx.cfg);
		rss_daemon_cleanup("rwc");
		return 0;
	}

	rwc_state_t st = {0};
	st.cfg = ctx.cfg;
	st.config_path = ctx.config_path;
	st.running = ctx.running;
	st.uvc_fd = -1;
	st.mic_fd = -1;

	/* Config */
	const char *dev = rss_config_get_str(ctx.cfg, "webcam", "device",
	                                     "/dev/video0");
	rss_strlcpy(st.uvc_device, dev, sizeof(st.uvc_device));

	st.buf_count = rss_config_get_int(ctx.cfg, "webcam", "buffers", 4);
	if (st.buf_count < 2) st.buf_count = 2;
	if (st.buf_count > RWC_MAX_BUFFERS) st.buf_count = RWC_MAX_BUFFERS;

	const char *jpeg_name = rss_config_get_str(ctx.cfg, "webcam",
	                                           "jpeg_stream", "jpeg0");
	const char *h264_name = rss_config_get_str(ctx.cfg, "webcam",
	                                           "h264_stream", "video0");
	st.audio_enabled = rss_config_get_bool(ctx.cfg, "webcam", "audio", true);
	const char *audio_name = rss_config_get_str(ctx.cfg, "webcam",
	                                            "audio_stream", "audio");

	/* Open UVC device */
	if (uvc_open(&st) < 0) {
		RSS_FATAL("cannot open UVC device %s", st.uvc_device);
		rss_config_free(ctx.cfg);
		rss_daemon_cleanup("rwc");
		return 1;
	}

	/* Open video rings (retry up to 30s) */
	for (int attempt = 0; attempt < 30 && *st.running; attempt++) {
		if (!st.jpeg_ring)
			st.jpeg_ring = try_open_ring(jpeg_name);
		if (!st.video_ring)
			st.video_ring = try_open_ring(h264_name);
		if (st.jpeg_ring || st.video_ring)
			break;
		RSS_DEBUG("waiting for video rings...");
		sleep(1);
	}

	if (!st.jpeg_ring && !st.video_ring) {
		RSS_FATAL("no video rings available");
		close(st.uvc_fd);
		rss_config_free(ctx.cfg);
		rss_daemon_cleanup("rwc");
		return 1;
	}

	/* Allocate video frame buffer */
	st.frame_buf_size = 512 * 1024;
	if (st.jpeg_ring) {
		const rss_ring_header_t *hdr = rss_ring_get_header(st.jpeg_ring);
		if (hdr->data_size > st.frame_buf_size)
			st.frame_buf_size = hdr->data_size;
	}
	if (st.video_ring) {
		const rss_ring_header_t *hdr = rss_ring_get_header(st.video_ring);
		if (hdr->data_size > st.frame_buf_size)
			st.frame_buf_size = hdr->data_size;
	}

	st.frame_buf = malloc(st.frame_buf_size);
	if (!st.frame_buf) {
		RSS_FATAL("frame buffer alloc failed (%u bytes)", st.frame_buf_size);
		close(st.uvc_fd);
		rss_config_free(ctx.cfg);
		rss_daemon_cleanup("rwc");
		return 1;
	}

	/* Audio setup */
	if (st.audio_enabled)
		audio_init(&st, audio_name);

	/* Control socket */
	rss_mkdir_p("/var/run/rss");
	st.ctrl = rss_ctrl_listen("/var/run/rss/rwc.sock");

	RSS_INFO("rwc running (jpeg=%s video=%s audio=%s)",
	         st.jpeg_ring ? "yes" : "no",
	         st.video_ring ? "yes" : "no",
	         st.audio_enabled ? "yes" : "no");

	/* ---- Main loop ---- */

	int reconnect_tick = 0;

	while (*st.running) {
		struct pollfd fds[2];
		int nfds = 0;

		fds[nfds].fd = st.uvc_fd;
		fds[nfds].events = POLLPRI;
		nfds++;

		if (st.ctrl) {
			int ctrl_fd = rss_ctrl_get_fd(st.ctrl);
			if (ctrl_fd >= 0) {
				fds[nfds].fd = ctrl_fd;
				fds[nfds].events = POLLIN;
				nfds++;
			}
		}

		int timeout = st.streaming ? 30 : 500;
		int n = poll(fds, nfds, timeout);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			RSS_ERROR("poll: %s", strerror(errno));
			break;
		}

		if (fds[0].revents & POLLPRI)
			process_events(&st);

		if (nfds > 1 && (fds[1].revents & POLLIN))
			rss_ctrl_accept_and_handle(st.ctrl, ctrl_handler, &st);

		if (st.streaming)
			deliver_frame(&st);

		if (st.audio_enabled)
			feed_audio(&st);

		/* Ring reconnection (~every 2s at 50ms/tick when streaming) */
		if (++reconnect_tick >= 40) {
			reconnect_tick = 0;

			if (!st.jpeg_ring)
				st.jpeg_ring = try_open_ring(jpeg_name);
			if (!st.video_ring)
				st.video_ring = try_open_ring(h264_name);
			if (st.audio_enabled && !st.audio_ring)
				st.audio_ring = try_open_ring(audio_name);

			check_ring_idle(&st.jpeg_ring, &st.jpeg_last_ws,
			                &st.jpeg_idle, "jpeg");
			check_ring_idle(&st.video_ring, &st.video_last_ws,
			                &st.video_idle, "video");
			if (st.audio_enabled)
				check_ring_idle(&st.audio_ring, &st.audio_last_ws,
				                &st.audio_idle, "audio");
		}
	}

	/* ---- Cleanup ---- */

	RSS_INFO("rwc shutting down");
	stop_streaming(&st);
	free(st.frame_buf);
	free(st.audio_buf);
	if (st.jpeg_ring)  rss_ring_close(st.jpeg_ring);
	if (st.video_ring) rss_ring_close(st.video_ring);
	if (st.audio_ring) rss_ring_close(st.audio_ring);
	if (st.mic_fd >= 0) close(st.mic_fd);
	if (st.ctrl) rss_ctrl_destroy(st.ctrl);
	if (st.uvc_fd >= 0) close(st.uvc_fd);
	rss_config_free(ctx.cfg);
	rss_daemon_cleanup("rwc");
	return 0;
}

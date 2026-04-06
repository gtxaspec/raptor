/*
 * rwc_main.c — Raptor Webcam Daemon
 *
 * Ring consumer that reads JPEG or H.264 frames and feeds them to the
 * Linux UVC gadget V4L2 interface, making the camera appear as a USB
 * webcam to the connected host.
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
    uint8_t cur_format;      /* RWC_FMT_MJPEG or RWC_FMT_H264 */
    uint8_t cur_frame;       /* 1-3 */
    uint32_t cur_interval;   /* 100ns units */
    struct uvc_streaming_control probe;
    struct uvc_streaming_control commit;
    uint8_t last_cs;         /* track CS for DATA event */

    /* Rings */
    rss_ring_t *jpeg_ring;
    rss_ring_t *video_ring;
    uint64_t read_seq;
    uint8_t *frame_buf;
    uint32_t frame_buf_size;

    /* Control socket */
    rss_ctrl_t *ctrl;
} rwc_state_t;

/* --------------------------------------------------------------------------
 * Streaming control fill
 */

static void fill_streaming_control(struct uvc_streaming_control *ctrl,
                                   uint8_t format_idx, uint8_t frame_idx,
                                   uint32_t interval)
{
    memset(ctrl, 0, sizeof(*ctrl));
    ctrl->bmHint = 1;

    /* Clamp format */
    if (format_idx < 1 || format_idx > RWC_NUM_FORMATS)
        format_idx = RWC_FMT_MJPEG;
    ctrl->bFormatIndex = format_idx;

    /* Clamp frame */
    if (frame_idx < 1 || frame_idx > RWC_NUM_FRAMES)
        frame_idx = RWC_FRAME_1080P;
    ctrl->bFrameIndex = frame_idx;

    /* Clamp interval */
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

/* Forward declarations */
static int start_streaming(rwc_state_t *st);
static void stop_streaming(rwc_state_t *st);

/* --------------------------------------------------------------------------
 * UVC event handlers
 */

static void handle_setup_event(rwc_state_t *st, const struct usb_ctrlrequest *req)
{
    struct uvc_request_data resp;
    memset(&resp, 0, sizeof(resp));

    uint8_t type = req->bRequestType;
    uint8_t request = req->bRequest;
    uint8_t cs = req->wValue >> 8;
    uint8_t intf = req->wIndex & 0xff;
    uint16_t wLength = req->wLength;

    /* Stall unsupported requests (control interface, unknown CS) */
    if (intf != UVC_INTF_STREAMING) {
        resp.length = -1;
        goto send;
    }

    if (cs != UVC_VS_PROBE_CONTROL && cs != UVC_VS_COMMIT_CONTROL) {
        resp.length = -1;
        goto send;
    }

    struct uvc_streaming_control *target =
        (cs == UVC_VS_PROBE_CONTROL) ? &st->probe : &st->commit;

    if (type & USB_DIR_IN) {
        /* GET requests: return current/min/max/def */
        uint16_t len = wLength < sizeof(*target) ? wLength : sizeof(*target);
        switch (request) {
        case UVC_GET_CUR:
            resp.length = len;
            memcpy(resp.data, target, len);
            break;
        case UVC_GET_MIN:
            fill_streaming_control((struct uvc_streaming_control *)resp.data,
                                   RWC_FMT_MJPEG, RWC_FRAME_360P,
                                   RWC_INTERVAL_15FPS);
            resp.length = len;
            break;
        case UVC_GET_MAX:
            fill_streaming_control((struct uvc_streaming_control *)resp.data,
                                   RWC_FMT_H264, RWC_FRAME_1080P,
                                   RWC_INTERVAL_30FPS);
            resp.length = len;
            break;
        case UVC_GET_DEF:
            fill_streaming_control((struct uvc_streaming_control *)resp.data,
                                   RWC_FMT_MJPEG, RWC_FRAME_1080P,
                                   RWC_INTERVAL_30FPS);
            resp.length = len;
            break;
        default:
            resp.length = -1;
            break;
        }
    } else {
        /* SET_CUR: accept data, track which CS for DATA event */
        st->last_cs = cs;
        resp.length = wLength;
    }

send:
    if (ioctl(st->uvc_fd, UVCIOC_SEND_RESPONSE, &resp) < 0)
        RSS_WARN("UVCIOC_SEND_RESPONSE: %s", strerror(errno));
}

static void handle_data_event(rwc_state_t *st, const struct uvc_request_data *data)
{
    const struct uvc_streaming_control *proposed =
        (const struct uvc_streaming_control *)data->data;

    struct uvc_streaming_control *target =
        (st->last_cs == UVC_VS_COMMIT_CONTROL) ? &st->commit : &st->probe;

    /* Clamp and store */
    fill_streaming_control(target,
                           proposed->bFormatIndex,
                           proposed->bFrameIndex,
                           proposed->dwFrameInterval);

    RSS_DEBUG("%s: proposed fmt=%u frm=%u int=%u → stored fmt=%u frm=%u int=%u maxvfs=%u maxpts=%u",
              st->last_cs == UVC_VS_COMMIT_CONTROL ? "COMMIT" : "PROBE",
              proposed->bFormatIndex, proposed->bFrameIndex,
              proposed->dwFrameInterval,
              target->bFormatIndex, target->bFrameIndex,
              target->dwFrameInterval,
              target->dwMaxVideoFrameSize,
              target->dwMaxPayloadTransferSize);

    /* For bulk mode: start streaming after COMMIT (no STREAMON event) */
    if (st->last_cs == UVC_VS_COMMIT_CONTROL && !st->streaming) {
        RSS_INFO("COMMIT received, starting streaming");
        if (start_streaming(st) < 0)
            RSS_ERROR("failed to start streaming after COMMIT");
    }
}

/* --------------------------------------------------------------------------
 * V4L2 streaming start / stop
 */

static int start_streaming(rwc_state_t *st)
{
    struct v4l2_requestbuffers rb;
    memset(&rb, 0, sizeof(rb));
    rb.count = st->buf_count;
    rb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    rb.memory = V4L2_MEMORY_MMAP;

    if (ioctl(st->uvc_fd, VIDIOC_REQBUFS, &rb) < 0) {
        RSS_ERROR("VIDIOC_REQBUFS: %s", strerror(errno));
        return -1;
    }
    st->buf_count = rb.count;

    for (int i = 0; i < st->buf_count; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(st->uvc_fd, VIDIOC_QUERYBUF, &buf) < 0) {
            RSS_ERROR("VIDIOC_QUERYBUF %d: %s", i, strerror(errno));
            return -1;
        }

        st->buffers[i].length = buf.length;
        st->buffers[i].start = mmap(NULL, buf.length,
                                    PROT_READ | PROT_WRITE, MAP_SHARED,
                                    st->uvc_fd, buf.m.offset);
        if (st->buffers[i].start == MAP_FAILED) {
            RSS_ERROR("mmap buffer %d: %s", i, strerror(errno));
            return -1;
        }

        if (ioctl(st->uvc_fd, VIDIOC_QBUF, &buf) < 0) {
            RSS_ERROR("VIDIOC_QBUF %d: %s", i, strerror(errno));
            return -1;
        }
    }

    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if (ioctl(st->uvc_fd, VIDIOC_STREAMON, &type) < 0) {
        RSS_ERROR("VIDIOC_STREAMON: %s", strerror(errno));
        return -1;
    }

    /* Apply committed format */
    st->cur_format = st->probe.bFormatIndex;
    st->cur_frame = st->probe.bFrameIndex;
    st->cur_interval = st->probe.dwFrameInterval;
    st->read_seq = 0;

    RSS_INFO("streaming started: %s %ux%u @ %u fps",
             st->cur_format == RWC_FMT_H264 ? "H.264" : "MJPEG",
             rwc_frames[st->cur_frame].width,
             rwc_frames[st->cur_frame].height,
             10000000 / st->cur_interval);

    st->streaming = true;
    return 0;
}

static void stop_streaming(rwc_state_t *st)
{
    if (!st->streaming)
        return;

    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ioctl(st->uvc_fd, VIDIOC_STREAMOFF, &type);

    for (int i = 0; i < st->buf_count; i++) {
        if (st->buffers[i].start && st->buffers[i].start != MAP_FAILED) {
            munmap(st->buffers[i].start, st->buffers[i].length);
            st->buffers[i].start = NULL;
        }
    }

    /* Free buffers */
    struct v4l2_requestbuffers rb;
    memset(&rb, 0, sizeof(rb));
    rb.count = 0;
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

        RSS_DEBUG("UVC event type=%u", ev.type);

        switch (ev.type) {
        case UVC_EVENT_CONNECT:
            RSS_INFO("USB connected (speed %d)", uvc_ev->speed);
            break;

        case UVC_EVENT_DISCONNECT:
            RSS_INFO("USB disconnected");
            stop_streaming(st);
            break;

        case UVC_EVENT_SETUP: {
            const struct usb_ctrlrequest *req = &uvc_ev->req;
            RSS_DEBUG("SETUP bRequestType=0x%02x bRequest=0x%02x wValue=0x%04x wIndex=0x%04x wLength=%u",
                      req->bRequestType, req->bRequest, req->wValue, req->wIndex, req->wLength);
            handle_setup_event(st, req);
            break;
        }

        case UVC_EVENT_DATA:
            RSS_DEBUG("DATA length=%d", uvc_ev->data.length);
            handle_data_event(st, &uvc_ev->data);
            break;

        case UVC_EVENT_STREAMON:
            RSS_INFO("UVC STREAMON");
            if (start_streaming(st) < 0)
                RSS_ERROR("failed to start streaming");
            break;

        case UVC_EVENT_STREAMOFF:
            RSS_INFO("UVC STREAMOFF");
            stop_streaming(st);
            break;

        default:
            RSS_DEBUG("unknown UVC event %u", ev.type);
            break;
        }
    }
}

/* --------------------------------------------------------------------------
 * Frame delivery
 */

static void deliver_frame(rwc_state_t *st)
{
    rss_ring_t *ring = NULL;

    if (st->cur_format == RWC_FMT_H264)
        ring = st->video_ring;
    else
        ring = st->jpeg_ring;

    if (!ring)
        return;

    uint32_t length = 0;
    rss_ring_slot_t meta;
    int ret = rss_ring_read(ring, &st->read_seq,
                            st->frame_buf, st->frame_buf_size,
                            &length, &meta);

    if (ret == RSS_EOVERFLOW && st->read_seq > 0) {
        st->read_seq--;
        ret = rss_ring_read(ring, &st->read_seq,
                            st->frame_buf, st->frame_buf_size,
                            &length, &meta);
    }

    if (ret != 0 || length == 0)
        return;

    /* Validate JPEG SOI marker */
    if (st->cur_format == RWC_FMT_MJPEG) {
        if (length < 2 || st->frame_buf[0] != 0xFF || st->frame_buf[1] != 0xD8) {
            RSS_DEBUG("invalid JPEG frame (len=%u hdr=0x%02x%02x)", length,
                      length > 0 ? st->frame_buf[0] : 0,
                      length > 1 ? st->frame_buf[1] : 0);
            return;
        }
    }

    /* Dequeue a V4L2 buffer */
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    buf.memory = V4L2_MEMORY_MMAP;

    if (ioctl(st->uvc_fd, VIDIOC_DQBUF, &buf) < 0) {
        if (errno != EAGAIN)
            RSS_DEBUG("VIDIOC_DQBUF: %s", strerror(errno));
        return;
    }

    /* Copy frame data */
    uint32_t copy_len = length;
    if (copy_len > st->buffers[buf.index].length) {
        RSS_WARN("frame truncated: %u > %zu", length, st->buffers[buf.index].length);
        copy_len = st->buffers[buf.index].length;
    }

    static int frame_count;
    if (++frame_count <= 5)
        RSS_DEBUG("frame %d: %u bytes → buf[%d] (%zu bytes)", frame_count, length,
                  buf.index, st->buffers[buf.index].length);

    memcpy(st->buffers[buf.index].start, st->frame_buf, copy_len);
    buf.bytesused = copy_len;

    /* Queue buffer back */
    if (ioctl(st->uvc_fd, VIDIOC_QBUF, &buf) < 0)
        RSS_WARN("VIDIOC_QBUF: %s", strerror(errno));
}

/* --------------------------------------------------------------------------
 * Ring management
 */

static rss_ring_t *try_open_ring(const char *name)
{
    rss_ring_t *ring = rss_ring_open(name);
    if (ring) {
        const rss_ring_header_t *hdr = rss_ring_get_header(ring);
        RSS_INFO("ring %s: %ux%u codec=%u", name, hdr->width, hdr->height, hdr->codec);
    }
    return ring;
}

/* --------------------------------------------------------------------------
 * UVC device open
 */

static int uvc_open(rwc_state_t *st)
{
    st->uvc_fd = open(st->uvc_device, O_RDWR | O_NONBLOCK);
    if (st->uvc_fd < 0) {
        RSS_ERROR("open %s: %s", st->uvc_device, strerror(errno));
        return -1;
    }

    /* Subscribe to UVC events */
    struct v4l2_event_subscription sub;
    memset(&sub, 0, sizeof(sub));

    static const uint32_t events[] = {
        UVC_EVENT_CONNECT, UVC_EVENT_DISCONNECT,
        UVC_EVENT_STREAMON, UVC_EVENT_STREAMOFF,
        UVC_EVENT_SETUP, UVC_EVENT_DATA,
    };

    for (int i = 0; i < (int)(sizeof(events) / sizeof(events[0])); i++) {
        sub.type = events[i];
        if (ioctl(st->uvc_fd, VIDIOC_SUBSCRIBE_EVENT, &sub) < 0) {
            RSS_ERROR("VIDIOC_SUBSCRIBE_EVENT %u: %s", events[i], strerror(errno));
            close(st->uvc_fd);
            st->uvc_fd = -1;
            return -1;
        }
    }

    /* Set initial probe to default */
    fill_streaming_control(&st->probe, RWC_FMT_MJPEG, RWC_FRAME_1080P,
                           RWC_INTERVAL_30FPS);
    st->commit = st->probe;

    RSS_INFO("UVC device %s opened", st->uvc_device);
    return 0;
}

/* --------------------------------------------------------------------------
 * Control socket handler
 */

static int ctrl_handler(const char *cmd_json, char *resp, int resp_size, void *ud)
{
    rwc_state_t *st = ud;

    int rc = rss_ctrl_handle_common(cmd_json, resp, resp_size, st->cfg, st->config_path);
    if (rc >= 0)
        return rc;

    /* Default: status */
    snprintf(resp, resp_size,
             "{\"status\":\"ok\",\"streaming\":%s,\"format\":\"%s\","
             "\"resolution\":\"%ux%u\",\"fps\":%u}",
             st->streaming ? "true" : "false",
             st->cur_format == RWC_FMT_H264 ? "h264" : "mjpeg",
             st->streaming ? rwc_frames[st->cur_frame].width : 0,
             st->streaming ? rwc_frames[st->cur_frame].height : 0,
             st->cur_interval ? 10000000 / st->cur_interval : 0);
    return (int)strlen(resp);
}

/* --------------------------------------------------------------------------
 * Main
 */

int main(int argc, char **argv)
{
    rss_daemon_ctx_t ctx;
    int ret = rss_daemon_init(&ctx, "rwc", argc, argv);
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

    /* Config */
    const char *dev = rss_config_get_str(ctx.cfg, "webcam", "device", "/dev/video0");
    rss_strlcpy(st.uvc_device, dev, sizeof(st.uvc_device));

    st.buf_count = rss_config_get_int(ctx.cfg, "webcam", "buffers", 4);
    if (st.buf_count < 2) st.buf_count = 2;
    if (st.buf_count > RWC_MAX_BUFFERS) st.buf_count = RWC_MAX_BUFFERS;

    const char *jpeg_name = rss_config_get_str(ctx.cfg, "webcam", "jpeg_stream", "jpeg0");
    const char *h264_name = rss_config_get_str(ctx.cfg, "webcam", "h264_stream", "video0");

    /* Open UVC device */
    if (uvc_open(&st) < 0) {
        RSS_FATAL("cannot open UVC device %s", st.uvc_device);
        rss_config_free(ctx.cfg);
        rss_daemon_cleanup("rwc");
        return 1;
    }

    /* Open rings (retry up to 30s) */
    for (int attempt = 0; attempt < 30 && *st.running; attempt++) {
        if (!st.jpeg_ring)
            st.jpeg_ring = try_open_ring(jpeg_name);
        if (!st.video_ring)
            st.video_ring = try_open_ring(h264_name);
        if (st.jpeg_ring || st.video_ring)
            break;
        RSS_DEBUG("waiting for rings...");
        sleep(1);
    }

    if (!st.jpeg_ring && !st.video_ring) {
        RSS_FATAL("no rings available");
        close(st.uvc_fd);
        rss_config_free(ctx.cfg);
        rss_daemon_cleanup("rwc");
        return 1;
    }

    /* Allocate frame buffer from ring header */
    st.frame_buf_size = 512 * 1024; /* 512KB default */
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
        RSS_FATAL("failed to allocate frame buffer (%u bytes)", st.frame_buf_size);
        close(st.uvc_fd);
        rss_config_free(ctx.cfg);
        rss_daemon_cleanup("rwc");
        return 1;
    }

    /* Control socket */
    rss_mkdir_p("/var/run/rss");
    st.ctrl = rss_ctrl_listen("/var/run/rss/rwc.sock");

    /* Main loop */
    RSS_INFO("rwc running (jpeg=%s video=%s)",
             st.jpeg_ring ? "yes" : "no",
             st.video_ring ? "yes" : "no");

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

        /* UVC events */
        if (fds[0].revents & POLLPRI)
            process_events(&st);

        /* Control socket */
        if (nfds > 1 && (fds[1].revents & POLLIN))
            rss_ctrl_accept_and_handle(st.ctrl, ctrl_handler, &st);

        /* Deliver frames when streaming */
        if (st.streaming)
            deliver_frame(&st);

        /* Ring reconnection (~every 2s) */
        if (++reconnect_tick >= 40) {
            reconnect_tick = 0;

            if (!st.jpeg_ring)
                st.jpeg_ring = try_open_ring(jpeg_name);
            if (!st.video_ring)
                st.video_ring = try_open_ring(h264_name);

            /* Check for idle rings */
            if (st.jpeg_ring) {
                const rss_ring_header_t *hdr = rss_ring_get_header(st.jpeg_ring);
                static uint64_t last_jpeg_ws;
                static int jpeg_idle;
                uint64_t ws = hdr->write_seq;
                if (ws == last_jpeg_ws)
                    jpeg_idle++;
                else
                    jpeg_idle = 0;
                last_jpeg_ws = ws;
                if (jpeg_idle >= 10) {
                    RSS_DEBUG("jpeg ring idle, closing");
                    rss_ring_close(st.jpeg_ring);
                    st.jpeg_ring = NULL;
                    jpeg_idle = 0;
                }
            }
            if (st.video_ring) {
                const rss_ring_header_t *hdr = rss_ring_get_header(st.video_ring);
                static uint64_t last_video_ws;
                static int video_idle;
                uint64_t ws = hdr->write_seq;
                if (ws == last_video_ws)
                    video_idle++;
                else
                    video_idle = 0;
                last_video_ws = ws;
                if (video_idle >= 10) {
                    RSS_DEBUG("video ring idle, closing");
                    rss_ring_close(st.video_ring);
                    st.video_ring = NULL;
                    video_idle = 0;
                }
            }
        }
    }

    /* Cleanup */
    RSS_INFO("rwc shutting down");
    stop_streaming(&st);
    free(st.frame_buf);
    if (st.jpeg_ring) rss_ring_close(st.jpeg_ring);
    if (st.video_ring) rss_ring_close(st.video_ring);
    if (st.ctrl) rss_ctrl_destroy(st.ctrl);
    close(st.uvc_fd);
    rss_config_free(ctx.cfg);
    rss_daemon_cleanup("rwc");
    return 0;
}

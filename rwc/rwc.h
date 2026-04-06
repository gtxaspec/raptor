/*
 * rwc.h — Raptor Webcam Daemon (UVC gadget)
 *
 * USB Video/Audio Class definitions and format tables for the RWC daemon.
 * UVC event types and structures are from the kernel's uvc.h header,
 * reproduced here for userspace use.
 */
#ifndef RWC_H
#define RWC_H

#include <stdint.h>
#include <stdbool.h>
#include <linux/videodev2.h>
#include <linux/usb/ch9.h>

/* --------------------------------------------------------------------------
 * UVC gadget userspace API (from kernel uvc.h)
 */

#define UVC_EVENT_CONNECT    (V4L2_EVENT_PRIVATE_START + 0)
#define UVC_EVENT_DISCONNECT (V4L2_EVENT_PRIVATE_START + 1)
#define UVC_EVENT_STREAMON   (V4L2_EVENT_PRIVATE_START + 2)
#define UVC_EVENT_STREAMOFF  (V4L2_EVENT_PRIVATE_START + 3)
#define UVC_EVENT_SETUP      (V4L2_EVENT_PRIVATE_START + 4)
#define UVC_EVENT_DATA       (V4L2_EVENT_PRIVATE_START + 5)

struct uvc_request_data {
	int32_t length;
	uint8_t data[60];
};

struct uvc_event {
	union {
		enum usb_device_speed speed;
		struct usb_ctrlrequest req;
		struct uvc_request_data data;
	};
};

#define UVCIOC_SEND_RESPONSE _IOW('U', 1, struct uvc_request_data)

/* --------------------------------------------------------------------------
 * UVC Video Streaming control (USB Video Class 1.0, Table 4-47)
 */

struct uvc_streaming_control {
	uint16_t bmHint;
	uint8_t  bFormatIndex;
	uint8_t  bFrameIndex;
	uint32_t dwFrameInterval;
	uint16_t wKeyFrameRate;
	uint16_t wPFrameRate;
	uint16_t wCompQuality;
	uint16_t wCompWindowSize;
	uint16_t wDelay;
	uint32_t dwMaxVideoFrameSize;
	uint32_t dwMaxPayloadTransferSize;
	uint32_t dwClockFrequency;
	uint8_t  bmFramingInfo;
	uint8_t  bPreferedVersion;
	uint8_t  bMinVersion;
	uint8_t  bMaxVersion;
} __attribute__((packed));

#define UVC_STREAMING_CONTROL_SIZE 26 /* UVC 1.0 probe/commit size */

/* UVC interface subclass */
#define UVC_INTF_CONTROL    0
#define UVC_INTF_STREAMING  1

/* UVC VS request codes */
#define UVC_VS_PROBE_CONTROL   0x01
#define UVC_VS_COMMIT_CONTROL  0x02

/* UVC request types */
#define UVC_SET_CUR 0x01
#define UVC_GET_CUR 0x81
#define UVC_GET_MIN 0x82
#define UVC_GET_MAX 0x83
#define UVC_GET_DEF 0x87

/* --------------------------------------------------------------------------
 * Format / frame tables (must match webcam.c descriptors)
 */

#define RWC_FMT_MJPEG  1
#define RWC_FMT_H264   2
#define RWC_NUM_FORMATS 2

#define RWC_FRAME_1080P 1
#define RWC_FRAME_720P  2
#define RWC_FRAME_360P  3
#define RWC_NUM_FRAMES  3

struct rwc_frame_info {
	uint16_t width;
	uint16_t height;
	uint32_t max_size;
};

static const struct rwc_frame_info rwc_frames[RWC_NUM_FRAMES + 1] = {
	[0]               = {   0,    0,                 0},
	[RWC_FRAME_1080P] = {1920, 1080, 1920 * 1080 * 2},
	[RWC_FRAME_720P]  = {1280,  720, 1280 *  720 * 2},
	[RWC_FRAME_360P]  = { 640,  360,  640 *  360 * 2},
};

/* Supported frame intervals (100ns units) */
#define RWC_INTERVAL_30FPS  333333
#define RWC_INTERVAL_25FPS  400000
#define RWC_INTERVAL_15FPS  666666

/* --------------------------------------------------------------------------
 * V4L2 buffer limits
 */

#define RWC_MAX_BUFFERS   8
#define RWC_AUDIO_BUF_SIZE 4096  /* audio read buffer */
#define RWC_AUDIO_DRAIN_MAX 64   /* max audio frames per loop iteration */

struct rwc_buffer {
	void   *start;
	size_t  length;
};

#endif /* RWC_H */

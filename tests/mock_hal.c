/*
 * mock_hal.c -- Minimal mock HAL for host-side ASan testing.
 *
 * Provides rss_hal_create/destroy/get_ops with stub implementations.
 * All ops return RSS_OK (or RSS_ERR_NOTSUP if NULL). enc_poll returns
 * -EAGAIN to avoid infinite loops in encoder threads.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <raptor_hal.h>

/* Minimal internal context */
struct rss_hal_ctx {
	const rss_hal_ops_t *ops;
	bool initialized;
};

/* ── Stub implementations ── */

static int mock_init(void *ctx, const rss_sensor_config_t *cfg)
{
	(void)ctx;
	(void)cfg;
	return RSS_OK;
}

static int mock_deinit(void *ctx)
{
	(void)ctx;
	return RSS_OK;
}

static int mock_ok(void *ctx, ...)
{
	(void)ctx;
	return RSS_OK;
}

static int mock_enc_poll(void *ctx, int chn, uint32_t timeout_ms)
{
	(void)ctx;
	(void)chn;
	/* Sleep briefly then return timeout to avoid busy spin */
	struct timespec ts = {.tv_sec = 0, .tv_nsec = timeout_ms * 1000000ULL};
	if (ts.tv_nsec > 100000000)
		ts.tv_nsec = 100000000; /* cap at 100ms */
	nanosleep(&ts, NULL);
	return -EAGAIN;
}

static int mock_enc_get_frame(void *ctx, int chn, rss_frame_t *frame)
{
	(void)ctx;
	(void)chn;
	(void)frame;
	return -EAGAIN;
}

static int mock_fs_create_channel(void *ctx, int chn, const rss_fs_config_t *cfg)
{
	(void)ctx;
	(void)chn;
	(void)cfg;
	return RSS_OK;
}

static int mock_enc_create_channel(void *ctx, int chn, const rss_video_config_t *cfg)
{
	(void)ctx;
	(void)chn;
	(void)cfg;
	return RSS_OK;
}

static int mock_osd_create_region(void *ctx, int *handle, const rss_osd_region_t *attr)
{
	(void)ctx;
	(void)attr;
	static int next_handle = 0;
	*handle = next_handle++;
	return RSS_OK;
}

static int mock_get_exposure(void *ctx, rss_exposure_t *exp)
{
	(void)ctx;
	memset(exp, 0, sizeof(*exp));
	exp->total_gain = 1000;
	exp->exposure_time = 33333;
	exp->ae_luma = 128;
	return RSS_OK;
}

static int mock_audio_init(void *ctx, const rss_audio_config_t *cfg)
{
	(void)ctx;
	(void)cfg;
	return RSS_OK;
}

static int mock_audio_read_frame(void *ctx, int dev, int chn, rss_audio_frame_t *frame, bool block)
{
	(void)ctx;
	(void)dev;
	(void)chn;
	(void)frame;
	(void)block;
	/* Sleep briefly, return no data */
	struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000};
	nanosleep(&ts, NULL);
	return -EAGAIN;
}

static const rss_hal_caps_t mock_caps = {
	.soc_name = "T31_MOCK",
	.sdk_version = "mock-1.0",
	.has_h265 = false,
};

static const rss_hal_caps_t *mock_get_caps(void *ctx)
{
	(void)ctx;
	return &mock_caps;
}

/* ── Static ops table ── */

static const rss_hal_ops_t mock_ops = {
	/* System */
	.init = mock_init,
	.deinit = mock_deinit,
	.get_caps = mock_get_caps,
	.bind = (void *)mock_ok,
	.unbind = (void *)mock_ok,

	/* Framesource */
	.fs_create_channel = (void *)mock_fs_create_channel,
	.fs_destroy_channel = (void *)mock_ok,
	.fs_enable_channel = (void *)mock_ok,
	.fs_disable_channel = (void *)mock_ok,
	.fs_set_fifo = (void *)mock_ok,
	.fs_set_frame_depth = (void *)mock_ok,

	/* Encoder */
	.enc_create_group = (void *)mock_ok,
	.enc_destroy_group = (void *)mock_ok,
	.enc_create_channel = (void *)mock_enc_create_channel,
	.enc_destroy_channel = (void *)mock_ok,
	.enc_register_channel = (void *)mock_ok,
	.enc_unregister_channel = (void *)mock_ok,
	.enc_start = (void *)mock_ok,
	.enc_stop = (void *)mock_ok,
	.enc_poll = mock_enc_poll,
	.enc_get_frame = (void *)mock_enc_get_frame,
	.enc_release_frame = (void *)mock_ok,
	.enc_request_idr = (void *)mock_ok,
	.enc_set_bitrate = (void *)mock_ok,
	.enc_set_gop = (void *)mock_ok,
	.enc_set_fps = (void *)mock_ok,
	.enc_set_bufshare = (void *)mock_ok,
	.enc_set_qp_bounds = (void *)mock_ok,
	.enc_get_avg_bitrate = (void *)mock_ok,

	/* ISP */
	.isp_set_brightness = (void *)mock_ok,
	.isp_set_contrast = (void *)mock_ok,
	.isp_set_saturation = (void *)mock_ok,
	.isp_set_sharpness = (void *)mock_ok,
	.isp_set_hflip = (void *)mock_ok,
	.isp_set_vflip = (void *)mock_ok,
	.isp_set_running_mode = (void *)mock_ok,
	.isp_set_sensor_fps = (void *)mock_ok,
	.isp_set_antiflicker = (void *)mock_ok,
	.isp_set_bypass = (void *)mock_ok,
	.isp_set_sinter_strength = (void *)mock_ok,
	.isp_set_temper_strength = (void *)mock_ok,
	.isp_get_exposure = (void *)mock_get_exposure,

	/* OSD */
	.osd_set_pool_size = (void *)mock_ok,
	.osd_create_group = (void *)mock_ok,
	.osd_destroy_group = (void *)mock_ok,
	.osd_create_region = (void *)mock_osd_create_region,
	.osd_destroy_region = (void *)mock_ok,
	.osd_register_region = (void *)mock_ok,
	.osd_unregister_region = (void *)mock_ok,
	.osd_set_region_attr = (void *)mock_ok,
	.osd_update_region_data = (void *)mock_ok,
	.osd_show_region = (void *)mock_ok,
	.osd_start = (void *)mock_ok,
	.osd_stop = (void *)mock_ok,

	/* Audio */
	.audio_init = (void *)mock_audio_init,
	.audio_deinit = (void *)mock_ok,
	.audio_set_volume = (void *)mock_ok,
	.audio_set_gain = (void *)mock_ok,
	.audio_read_frame = (void *)mock_audio_read_frame,
	.audio_release_frame = (void *)mock_ok,
};

/* ── Public API matching raptor_hal.h ── */

rss_hal_ctx_t *rss_hal_create(void)
{
	rss_hal_ctx_t *ctx = calloc(1, sizeof(*ctx));
	if (ctx)
		ctx->ops = &mock_ops;
	return ctx;
}

void rss_hal_destroy(rss_hal_ctx_t *ctx)
{
	free(ctx);
}

const rss_hal_ops_t *rss_hal_get_ops(rss_hal_ctx_t *ctx)
{
	return ctx ? ctx->ops : NULL;
}

int rss_hal_get_imp_version(char *buf, int size)
{
	snprintf(buf, size, "MOCK-IMP-1.0.0");
	return 0;
}

int rss_hal_get_sysutils_version(char *buf, int size)
{
	snprintf(buf, size, "MOCK-SYSUTILS-1.0.0");
	return 0;
}

const char *rss_hal_get_cpu_info(void)
{
	return "MOCK-CPU";
}

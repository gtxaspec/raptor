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
#include <stdarg.h>
#include <errno.h>
#include <time.h>
#include <raptor_hal.h>

/* ── Log callback (matches hal_common.c) ── */

static const char *mock_level_str[] = {"FTL", "ERR", "WRN", "INF", "DBG"};

static void mock_log_stderr(int level, const char *file, int line, const char *fmt, ...)
{
	if (level < 0) level = 0;
	if (level > 4) level = 4;
	fprintf(stderr, "[HAL %s] %s:%d: ", mock_level_str[level], file, line);
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

rss_hal_log_func_t rss_hal_log_fn = mock_log_stderr;

void rss_hal_set_log_func(rss_hal_log_func_t func)
{
	rss_hal_log_fn = func ? func : mock_log_stderr;
}

/* Minimal internal context */
struct rss_hal_ctx {
	const rss_hal_ops_t *ops;
	bool initialized;
};

/* ── Stub implementations ── */

static int mock_init(void *ctx, const rss_multi_sensor_config_t *cfg)
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

static int mock_enc_get_channel_attr(void *ctx, int chn, rss_video_config_t *cfg)
{
	(void)ctx;
	(void)chn;
	memset(cfg, 0, sizeof(*cfg));
	cfg->codec = RSS_CODEC_H264;
	cfg->width = 1920;
	cfg->height = 1080;
	cfg->profile = 2; /* high */
	cfg->rc_mode = RSS_RC_VBR;
	cfg->bitrate = 2000000;
	cfg->max_bitrate = 4000000;
	cfg->init_qp = -1;
	cfg->min_qp = 15;
	cfg->max_qp = 45;
	cfg->fps_num = 25;
	cfg->fps_den = 1;
	cfg->gop_length = 50;
	cfg->gop_mode = RSS_GOP_DEFAULT;
	cfg->max_same_scene_cnt = 2;
	cfg->buf_size = 1048576;
	return RSS_OK;
}

static int mock_enc_get_fps(void *ctx, int chn, uint32_t *fps_num, uint32_t *fps_den)
{
	(void)ctx;
	(void)chn;
	*fps_num = 25;
	*fps_den = 1;
	return RSS_OK;
}

static int mock_enc_get_gop_attr(void *ctx, int chn, uint32_t *gop_length)
{
	(void)ctx;
	(void)chn;
	*gop_length = 50;
	return RSS_OK;
}

static int mock_enc_get_gop_mode(void *ctx, int chn, rss_gop_mode_t *mode)
{
	(void)ctx;
	(void)chn;
	*mode = RSS_GOP_DEFAULT;
	return RSS_OK;
}

static int mock_enc_get_rc_options(void *ctx, int chn, uint32_t *options)
{
	(void)ctx;
	(void)chn;
	*options = 0;
	return RSS_OK;
}

static int mock_enc_get_max_same_scene_cnt(void *ctx, int chn, uint32_t *count)
{
	(void)ctx;
	(void)chn;
	*count = 2;
	return RSS_OK;
}

static int mock_enc_get_color2grey(void *ctx, int chn, bool *enable)
{
	(void)ctx;
	(void)chn;
	*enable = false;
	return RSS_OK;
}

static int mock_enc_get_mbrc(void *ctx, int chn, bool *enable)
{
	(void)ctx;
	(void)chn;
	*enable = false;
	return RSS_OK;
}

static int mock_enc_get_qpg_mode(void *ctx, int chn, int *mode)
{
	(void)ctx;
	(void)chn;
	*mode = 0;
	return RSS_OK;
}

static int mock_enc_get_stream_buf_size(void *ctx, int chn, uint32_t *size)
{
	(void)ctx;
	(void)chn;
	*size = 1048576;
	return RSS_OK;
}

static int mock_enc_get_jpeg_qp(void *ctx, int chn, int *qp)
{
	(void)ctx;
	(void)chn;
	*qp = 75;
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

static int mock_isp_get_u8(void *ctx, uint8_t *val)
{
	(void)ctx;
	*val = 128;
	return RSS_OK;
}

static int mock_isp_get_hvflip(void *ctx, int *hflip, int *vflip)
{
	(void)ctx;
	*hflip = 0;
	*vflip = 0;
	return RSS_OK;
}

static int mock_isp_get_ae_comp(void *ctx, int *val)
{
	(void)ctx;
	*val = 128;
	return RSS_OK;
}

static int mock_isp_get_u32(void *ctx, uint32_t *val)
{
	(void)ctx;
	*val = 1024;
	return RSS_OK;
}

static int mock_isp_get_wb(void *ctx, rss_wb_config_t *wb_cfg)
{
	(void)ctx;
	wb_cfg->mode = RSS_WB_AUTO;
	wb_cfg->r_gain = 256;
	wb_cfg->g_gain = 256;
	wb_cfg->b_gain = 256;
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

static int mock_ivs_get_result(void *ctx, int chn, void **result)
{
	(void)ctx;
	(void)chn;
	*result = NULL;
	return -EAGAIN;
}

static const rss_hal_caps_t mock_caps = {
	.soc_name = "T31_MOCK",
	.sdk_version = "mock-1.0",
	.has_h265 = false,
	.has_smartp_gop = true,
	.has_rc_options = true,
	.has_roi = true,
	.has_h264_trans = true,
	.has_h265_trans = true,
	.has_super_frame = true,
	.has_gop_attr = true,
	.has_set_bitrate = true,
	.has_stream_buf_size = true,
	.has_color2grey = true,
	.has_mbrc = true,
	.has_qpg_mode = true,
	.has_jpeg_qp = true,
	.has_sinter = true,
	.has_temper = true,
	.has_ae_comp = true,
	.has_max_gain = true,
	.has_bcsh_hue = true,
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
	.enc_set_rc_mode = (void *)mock_ok,
	.enc_set_bitrate = (void *)mock_ok,
	.enc_set_gop = (void *)mock_ok,
	.enc_set_fps = (void *)mock_ok,
	.enc_set_bufshare = (void *)mock_ok,
	.enc_get_channel_attr = mock_enc_get_channel_attr,
	.enc_get_fps = mock_enc_get_fps,
	.enc_get_gop_attr = mock_enc_get_gop_attr,
	.enc_set_gop_attr = (void *)mock_ok,
	.enc_get_avg_bitrate = (void *)mock_ok,
	.enc_set_qp_bounds = (void *)mock_ok,
	.enc_set_stream_buf_size = (void *)mock_ok,
	.enc_get_stream_buf_size = mock_enc_get_stream_buf_size,
	.enc_set_gop_mode = (void *)mock_ok,
	.enc_get_gop_mode = mock_enc_get_gop_mode,
	.enc_set_rc_options = (void *)mock_ok,
	.enc_get_rc_options = mock_enc_get_rc_options,
	.enc_set_max_same_scene_cnt = (void *)mock_ok,
	.enc_get_max_same_scene_cnt = mock_enc_get_max_same_scene_cnt,
	.enc_set_color2grey = (void *)mock_ok,
	.enc_get_color2grey = mock_enc_get_color2grey,
	.enc_set_roi = (void *)mock_ok,
	.enc_set_mbrc = (void *)mock_ok,
	.enc_get_mbrc = mock_enc_get_mbrc,
	.enc_set_qpg_mode = (void *)mock_ok,
	.enc_get_qpg_mode = mock_enc_get_qpg_mode,
	.enc_set_super_frame = (void *)mock_ok,
	.enc_set_h264_trans = (void *)mock_ok,
	.enc_get_h264_trans = (void *)mock_ok,
	.enc_set_h265_trans = (void *)mock_ok,
	.enc_get_h265_trans = (void *)mock_ok,
	.enc_set_jpeg_qp = (void *)mock_ok,
	.enc_get_jpeg_qp = mock_enc_get_jpeg_qp,

	/* ISP setters */
	.isp_set_brightness = (void *)mock_ok,
	.isp_set_contrast = (void *)mock_ok,
	.isp_set_saturation = (void *)mock_ok,
	.isp_set_sharpness = (void *)mock_ok,
	.isp_set_hue = (void *)mock_ok,
	.isp_set_hflip = (void *)mock_ok,
	.isp_set_vflip = (void *)mock_ok,
	.isp_set_running_mode = (void *)mock_ok,
	.isp_set_sensor_fps = (void *)mock_ok,
	.isp_set_antiflicker = (void *)mock_ok,
	.isp_set_bypass = (void *)mock_ok,
	.isp_set_sinter_strength = (void *)mock_ok,
	.isp_set_temper_strength = (void *)mock_ok,
	.isp_set_ae_comp = (void *)mock_ok,
	.isp_set_max_again = (void *)mock_ok,
	.isp_set_max_dgain = (void *)mock_ok,
	.isp_set_wb = (void *)mock_ok,

	/* ISP getters */
	.isp_get_exposure = (void *)mock_get_exposure,
	.isp_get_brightness = (void *)mock_isp_get_u8,
	.isp_get_contrast = (void *)mock_isp_get_u8,
	.isp_get_saturation = (void *)mock_isp_get_u8,
	.isp_get_sharpness = (void *)mock_isp_get_u8,
	.isp_get_hue = (void *)mock_isp_get_u8,
	.isp_get_hvflip = mock_isp_get_hvflip,
	.isp_get_ae_comp = mock_isp_get_ae_comp,
	.isp_get_max_again = (void *)mock_isp_get_u32,
	.isp_get_max_dgain = (void *)mock_isp_get_u32,
	.isp_get_sinter_strength = (void *)mock_isp_get_u8,
	.isp_get_temper_strength = (void *)mock_isp_get_u8,
	.isp_get_wb = mock_isp_get_wb,

	/* ISP multi-sensor setters */
	.isp_set_brightness_n = (void *)mock_ok,
	.isp_set_contrast_n = (void *)mock_ok,
	.isp_set_saturation_n = (void *)mock_ok,
	.isp_set_sharpness_n = (void *)mock_ok,
	.isp_set_hue_n = (void *)mock_ok,
	.isp_set_hflip_n = (void *)mock_ok,
	.isp_set_vflip_n = (void *)mock_ok,
	.isp_set_running_mode_n = (void *)mock_ok,
	.isp_set_sensor_fps_n = (void *)mock_ok,
	.isp_set_antiflicker_n = (void *)mock_ok,
	.isp_set_sinter_strength_n = (void *)mock_ok,
	.isp_set_temper_strength_n = (void *)mock_ok,
	.isp_set_ae_comp_n = (void *)mock_ok,
	.isp_set_max_again_n = (void *)mock_ok,
	.isp_set_max_dgain_n = (void *)mock_ok,
	.isp_get_exposure_n = (void *)mock_get_exposure,

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

	/* Audio input */
	.audio_init = (void *)mock_audio_init,
	.audio_deinit = (void *)mock_ok,
	.audio_set_volume = (void *)mock_ok,
	.audio_set_gain = (void *)mock_ok,
	.audio_enable_ns = (void *)mock_ok,
	.audio_disable_ns = (void *)mock_ok,
	.audio_enable_hpf = (void *)mock_ok,
	.audio_disable_hpf = (void *)mock_ok,
	.audio_enable_agc = (void *)mock_ok,
	.audio_disable_agc = (void *)mock_ok,
	.audio_read_frame = (void *)mock_audio_read_frame,
	.audio_release_frame = (void *)mock_ok,
	.audio_enable_aec = (void *)mock_ok,
	.audio_disable_aec = (void *)mock_ok,
	.audio_get_volume = (void *)mock_ok,
	.audio_get_gain = (void *)mock_ok,
	.audio_set_mute = (void *)mock_ok,
	.audio_set_alc_gain = (void *)mock_ok,

	/* Audio output */
	.ao_init = (void *)mock_ok,
	.ao_deinit = (void *)mock_ok,
	.ao_set_volume = (void *)mock_ok,
	.ao_set_gain = (void *)mock_ok,
	.ao_send_frame = (void *)mock_ok,
	.ao_pause = (void *)mock_ok,
	.ao_resume = (void *)mock_ok,
	.ao_clear_buf = (void *)mock_ok,

	/* Audio encoding */
	.aenc_create_channel = (void *)mock_ok,
	.aenc_destroy_channel = (void *)mock_ok,
	.aenc_send_frame = (void *)mock_ok,

	/* Audio decoding */
	.adec_create_channel = (void *)mock_ok,
	.adec_destroy_channel = (void *)mock_ok,
	.adec_send_stream = (void *)mock_ok,
	.adec_clear_buf = (void *)mock_ok,

	/* IVS */
	.ivs_create_group = (void *)mock_ok,
	.ivs_destroy_group = (void *)mock_ok,
	.ivs_create_channel = (void *)mock_ok,
	.ivs_destroy_channel = (void *)mock_ok,
	.ivs_register_channel = (void *)mock_ok,
	.ivs_unregister_channel = (void *)mock_ok,
	.ivs_start = (void *)mock_ok,
	.ivs_stop = (void *)mock_ok,
	.ivs_poll_result = (void *)mock_ok,
	.ivs_get_result = mock_ivs_get_result,
	.ivs_release_result = (void *)mock_ok,
	.ivs_get_param = (void *)mock_ok,
	.ivs_set_param = (void *)mock_ok,
	.ivs_release_data = (void *)mock_ok,

	/* GPIO / IR-cut */
	.gpio_set = (void *)mock_ok,
	.gpio_get = (void *)mock_ok,
	.ircut_set = (void *)mock_ok,
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

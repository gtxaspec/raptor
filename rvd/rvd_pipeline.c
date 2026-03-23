/*
 * rvd_pipeline.c -- HAL pipeline setup and teardown
 *
 * Reads config, initializes the sensor/ISP, creates framesource channels,
 * encoder groups/channels, OSD groups, binds the pipeline, and creates
 * SHM ring buffers.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rvd.h"

/* Map HAL profile enum (0=base,1=main,2=high) to H.264 profile_idc */
static uint8_t rvd_profile_idc(int profile)
{
	switch (profile) {
	case 0:  return 66;   /* Baseline */
	case 1:  return 77;   /* Main */
	case 2:  return 100;  /* High */
	default: return 100;
	}
}

/* Derive H.264 level_idc from resolution (Table A-1 in H.264 spec) */
static uint8_t rvd_level_idc(int width, int height)
{
	int macroblocks = ((width + 15) / 16) * ((height + 15) / 16);
	if (macroblocks <= 99)     return 10;  /* 1.0: up to 176x144 (QCIF) */
	if (macroblocks <= 396)    return 13;  /* 1.3: up to 352x288 (CIF) */
	if (macroblocks <= 792)    return 21;  /* 2.1: up to 480x360 */
	if (macroblocks <= 1620)   return 30;  /* 3.0: up to 720x576 */
	if (macroblocks <= 3600)   return 31;  /* 3.1: up to 1280x720 */
	if (macroblocks <= 5120)   return 32;  /* 3.2: up to 1280x1024 */
	if (macroblocks <= 8192)   return 40;  /* 4.0: up to 1920x1080 */
	if (macroblocks <= 8704)   return 42;  /* 4.2: up to 2048x1088 */
	if (macroblocks <= 22080)  return 50;  /* 5.0: up to 2560x1920 */
	if (macroblocks <= 36864)  return 51;  /* 5.1: up to 4096x2160 */
	return 52;                             /* 5.2 */
}

/* Parse codec string from config */
static rss_codec_t parse_codec(const char *s)
{
	if (!s) return RSS_CODEC_H264;
	if (strcasecmp(s, "h265") == 0 || strcasecmp(s, "hevc") == 0)
		return RSS_CODEC_H265;
	if (strcasecmp(s, "jpeg") == 0 || strcasecmp(s, "mjpeg") == 0)
		return RSS_CODEC_JPEG;
	return RSS_CODEC_H264;
}

/* Parse rate control mode string */
static rss_rc_mode_t parse_rc_mode(const char *s)
{
	if (!s) return RSS_RC_VBR;
	if (strcasecmp(s, "cbr") == 0)       return RSS_RC_CBR;
	if (strcasecmp(s, "fixqp") == 0)     return RSS_RC_FIXQP;
	if (strcasecmp(s, "capped_vbr") == 0) return RSS_RC_CAPPED_VBR;
	return RSS_RC_VBR;
}

/* Load one stream's config from INI section */
static void load_stream_config(rss_config_t *cfg, const char *section,
			       rvd_stream_t *s, int default_w, int default_h,
			       int default_fps, int default_br)
{
	int w = rss_config_get_int(cfg, section, "width", default_w);
	int h = rss_config_get_int(cfg, section, "height", default_h);
	int fps = rss_config_get_int(cfg, section, "fps", default_fps);

	/* Framesource config */
	s->fs_cfg = (rss_fs_config_t){
		.width   = w,
		.height  = h,
		.pixfmt  = RSS_PIXFMT_NV12,
		.fps_num = fps,
		.fps_den = 1,
		.nr_vbs  = rss_config_get_int(cfg, section, "nr_vbs", 2),
	};

	/* Encoder config */
	s->enc_cfg = (rss_video_config_t){
		.codec       = parse_codec(rss_config_get_str(cfg, section, "codec", "h264")),
		.width       = w,
		.height      = h,
		.profile     = rss_config_get_int(cfg, section, "profile", 2),
		.rc_mode     = parse_rc_mode(rss_config_get_str(cfg, section, "rc_mode", "vbr")),
		.bitrate     = rss_config_get_int(cfg, section, "bitrate", default_br),
		.max_bitrate = rss_config_get_int(cfg, section, "max_bitrate", 0),
		.fps_num     = fps,
		.fps_den     = 1,
		.gop_length  = rss_config_get_int(cfg, section, "gop", fps * 2),
		.init_qp     = -1,
		.min_qp      = rss_config_get_int(cfg, section, "min_qp", 15),
		.max_qp      = rss_config_get_int(cfg, section, "max_qp", 45),
	};
}

int rvd_pipeline_init(rvd_state_t *st)
{
	rss_config_t *cfg = st->cfg;
	int ret;

	/* ── 1. Create HAL ── */
	st->hal_ctx = rss_hal_create();
	if (!st->hal_ctx) {
		RSS_FATAL("rss_hal_create failed");
		return RSS_ERR;
	}
	st->ops = rss_hal_get_ops(st->hal_ctx);

	/* ── 2. Sensor config ── */
	rss_sensor_config_t sensor = {0};
	rss_strlcpy(sensor.name,
		    rss_config_get_str(cfg, "sensor", "name", "gc2053"),
		    sizeof(sensor.name));
	sensor.i2c_addr    = rss_config_get_int(cfg, "sensor", "i2c_addr", 0x37);
	sensor.i2c_adapter = rss_config_get_int(cfg, "sensor", "i2c_adapter", 0);
	sensor.rst_gpio    = rss_config_get_int(cfg, "sensor", "rst_gpio", -1);
	sensor.pwdn_gpio   = rss_config_get_int(cfg, "sensor", "pwdn_gpio", -1);
	sensor.power_gpio  = rss_config_get_int(cfg, "sensor", "power_gpio", -1);

	/* ── 3. Init HAL (brings up ISP + sensor) ── */
	ret = RSS_HAL_CALL(st->ops, init, st->hal_ctx, &sensor);
	if (ret != RSS_OK) {
		RSS_FATAL("HAL init failed: %d", ret);
		return ret;
	}

	/* Check caps for codec fallback */
	const rss_hal_caps_t *caps = st->ops->get_caps
		? st->ops->get_caps(st->hal_ctx) : NULL;

	/* ── 3b. Set sensor FPS (required — SDK won't encode without this) ── */
	int target_fps = rss_config_get_int(cfg, "stream0", "fps", 25);
	ret = RSS_HAL_CALL(st->ops, isp_set_sensor_fps, st->hal_ctx,
			   target_fps, 1);
	if (ret != RSS_OK)
		RSS_WARN("isp_set_sensor_fps failed: %d (non-fatal)", ret);

	/* ── 3c. ISP tuning defaults (match prudynt init sequence) ── */
	RSS_HAL_CALL(st->ops, isp_set_brightness, st->hal_ctx, 128);
	RSS_HAL_CALL(st->ops, isp_set_contrast, st->hal_ctx, 128);
	RSS_HAL_CALL(st->ops, isp_set_saturation, st->hal_ctx, 128);
	RSS_HAL_CALL(st->ops, isp_set_sharpness, st->hal_ctx, 128);
	RSS_HAL_CALL(st->ops, isp_set_sinter_strength, st->hal_ctx, 128);
	RSS_HAL_CALL(st->ops, isp_set_temper_strength, st->hal_ctx, 128);
	RSS_HAL_CALL(st->ops, isp_set_running_mode, st->hal_ctx, RSS_ISP_DAY);
	ret = RSS_HAL_CALL(st->ops, isp_set_bypass, st->hal_ctx, 1);
	RSS_DEBUG("isp_set_bypass returned %d", ret);
	RSS_HAL_CALL(st->ops, isp_set_antiflicker, st->hal_ctx, RSS_ANTIFLICKER_60HZ);
	RSS_HAL_CALL(st->ops, isp_set_hflip, st->hal_ctx, 0);
	RSS_HAL_CALL(st->ops, isp_set_vflip, st->hal_ctx, 0);

	/* ── 3d. Read actual sensor resolution from /proc ── */
	int sensor_w = 0, sensor_h = 0;
	{
		char *s;
		s = rss_read_file("/proc/jz/sensor/width", NULL);
		if (s) { sensor_w = atoi(s); free(s); }
		s = rss_read_file("/proc/jz/sensor/height", NULL);
		if (s) { sensor_h = atoi(s); free(s); }
	}
	if (sensor_w > 0 && sensor_h > 0)
		RSS_INFO("sensor resolution: %dx%d", sensor_w, sensor_h);
	else
		RSS_WARN("could not read sensor resolution from /proc");

	/* ── 4. Load stream configs ── */
	load_stream_config(cfg, "stream0", &st->streams[0],
			   1920, 1080, 25, 2000000);
	st->streams[0].chn = 0;
	st->stream_count = 1;

	/* Sub stream (optional) */
	if (rss_config_get_bool(cfg, "stream1", "enabled", true)) {
		load_stream_config(cfg, "stream1", &st->streams[1],
				   640, 360, 25, 500000);
		st->streams[1].chn = 1;
		st->stream_count = 2;
	}

	/* JPEG snapshot channel (optional).
	 * Vendor SDK uses encoder channels 4+ for JPEG, registers into
	 * video group 0, binds to same framesource as stream0. */
	st->jpeg_stream = -1;
	if (rss_config_get_bool(cfg, "jpeg", "enabled", true)) {
		int ji = st->stream_count;
		st->jpeg_quality = rss_config_get_int(cfg, "jpeg", "quality", 75);
		int jpeg_fps = rss_config_get_int(cfg, "jpeg", "fps", 1);

		st->streams[ji].enc_cfg = (rss_video_config_t){
			.codec   = RSS_CODEC_JPEG,
			.width   = st->streams[0].enc_cfg.width,
			.height  = st->streams[0].enc_cfg.height,
			.fps_num = jpeg_fps,
			.fps_den = 1,
			.bitrate = 0,
		};
		st->streams[ji].fs_cfg = st->streams[0].fs_cfg;
		st->streams[ji].chn = 4;  /* SDK convention: JPEG at channel 4+ */
		st->streams[ji].is_jpeg = true;
		st->jpeg_stream = ji;
		st->stream_count = ji + 1;

		snprintf(st->jpeg_path, sizeof(st->jpeg_path), "/tmp/snapshot.jpg");
		RSS_INFO("jpeg: %ux%u @ %d fps, quality %d (enc chn 4)",
			 st->streams[ji].enc_cfg.width,
			 st->streams[ji].enc_cfg.height,
			 jpeg_fps, st->jpeg_quality);
	}

	/* H.265 fallback on SoCs without support */
	if (caps && !caps->has_h265) {
		for (int i = 0; i < st->stream_count; i++) {
			if (st->streams[i].enc_cfg.codec == RSS_CODEC_H265) {
				RSS_WARN("stream%d: H.265 not supported, falling back to H.264", i);
				st->streams[i].enc_cfg.codec = RSS_CODEC_H264;
			}
		}
	}

	/* ── 4b. Enable scaler for streams at lower resolution than sensor ── */
	if (sensor_w > 0 && sensor_h > 0) {
		for (int i = 0; i < st->stream_count; i++) {
			if (st->streams[i].is_jpeg) continue; /* shares stream0's FS */
			rss_fs_config_t *fs = &st->streams[i].fs_cfg;
			if (fs->width != sensor_w || fs->height != sensor_h) {
				fs->scaler.enable = true;
				fs->scaler.out_width = fs->width;
				fs->scaler.out_height = fs->height;
				RSS_INFO("stream%d: scaler %dx%d -> %dx%d",
					 i, sensor_w, sensor_h,
					 fs->width, fs->height);
			}
		}
	}

	/* ── 5. Create framesource channels ── */
	/* JPEG shares framesource with stream0 — skip FS creation for it */
	for (int i = 0; i < st->stream_count; i++) {
		if (st->streams[i].is_jpeg)
			continue;
		ret = RSS_HAL_CALL(st->ops, fs_create_channel, st->hal_ctx,
				   i, &st->streams[i].fs_cfg);
		if (ret != RSS_OK) {
			RSS_FATAL("fs_create_channel(%d) failed: %d", i, ret);
			return ret;
		}
		/* Set fifo and frame depth to 0 (prudynt pattern — required for frames to flow) */
		RSS_HAL_CALL(st->ops, fs_set_fifo, st->hal_ctx, i, 0);
		RSS_HAL_CALL(st->ops, fs_set_frame_depth, st->hal_ctx, i, 0);
	}

	/* ── 6. (OSD deferred to Phase 4) ── */

	/* ── 7. Create encoder groups and channels ── */
	/* Video streams: create group, create chn, register chn in group.
	 * JPEG: no group (registers into group 0), uses enc chn 4+ per SDK convention. */
	for (int i = 0; i < st->stream_count; i++) {
		int chn = st->streams[i].chn;

		if (st->streams[i].is_jpeg) {
			/* Optional buffer sharing (before CreateChn per prudynt order) */
			if (rss_config_get_bool(cfg, "jpeg", "bufshare", true)) {
				ret = RSS_HAL_CALL(st->ops, enc_set_bufshare,
						   st->hal_ctx, chn, 0);
				if (ret == RSS_OK)
					RSS_DEBUG("jpeg bufshare chn %d -> group 0", chn);
				else
					RSS_WARN("jpeg bufshare failed: %d (non-fatal)", ret);
			}

			st->streams[i].enc_cfg.init_qp = st->jpeg_quality;
			ret = RSS_HAL_CALL(st->ops, enc_create_channel,
					   st->hal_ctx, chn,
					   &st->streams[i].enc_cfg);
			if (ret != RSS_OK) {
				RSS_FATAL("enc_create_channel(%d/JPEG) failed: %d",
					  chn, ret);
				return ret;
			}

			/* Register JPEG into video group 0 */
			ret = RSS_HAL_CALL(st->ops, enc_register_channel,
					   st->hal_ctx, 0, chn);
			if (ret != RSS_OK) {
				RSS_FATAL("enc_register_channel(0, %d) failed: %d",
					  chn, ret);
				return ret;
			}
		} else {
			ret = RSS_HAL_CALL(st->ops, enc_create_group,
					   st->hal_ctx, chn);
			if (ret != RSS_OK) {
				RSS_FATAL("enc_create_group(%d) failed: %d", chn, ret);
				return ret;
			}

			ret = RSS_HAL_CALL(st->ops, enc_create_channel,
					   st->hal_ctx, chn,
					   &st->streams[i].enc_cfg);
			if (ret != RSS_OK) {
				RSS_FATAL("enc_create_channel(%d) failed: %d", chn, ret);
				return ret;
			}

			ret = RSS_HAL_CALL(st->ops, enc_register_channel,
					   st->hal_ctx, chn, chn);
			if (ret != RSS_OK) {
				RSS_FATAL("enc_register_channel(%d) failed: %d", chn, ret);
				return ret;
			}
		}
	}

	/* ── 8. Bind pipeline: FS → Encoder (group level) ── */
	/* JPEG is registered in group 0 — no separate bind needed.
	 * The SDK bind is per-group, not per-channel. */
	for (int i = 0; i < st->stream_count; i++) {
		if (st->streams[i].is_jpeg)
			continue;
		int chn = st->streams[i].chn;
		rss_cell_t fs_out = { RSS_DEV_FS,  chn, 0 };
		rss_cell_t enc_in = { RSS_DEV_ENC, chn, 0 };

		ret = RSS_HAL_CALL(st->ops, bind, st->hal_ctx, &fs_out, &enc_in);
		if (ret != RSS_OK) {
			RSS_FATAL("bind FS(%d)->ENC(%d) failed: %d", chn, chn, ret);
			return ret;
		}
	}

	/* ── 9. Enable framesource, start OSD, start encoder ── */
	for (int i = 0; i < st->stream_count; i++) {
		int chn = st->streams[i].chn;

		/* JPEG shares framesource with stream0 — don't enable twice */
		if (!st->streams[i].is_jpeg) {
			ret = RSS_HAL_CALL(st->ops, fs_enable_channel, st->hal_ctx, chn);
			if (ret != RSS_OK) {
				RSS_FATAL("fs_enable_channel(%d) failed: %d", chn, ret);
				return ret;
			}
		}

		ret = RSS_HAL_CALL(st->ops, enc_start, st->hal_ctx, chn);
		if (ret != RSS_OK) {
			RSS_FATAL("enc_start(%d) failed: %d", i, ret);
			return ret;
		}

		st->streams[i].enabled = true;
		if (st->streams[i].is_jpeg) {
			RSS_INFO("stream%d: %ux%u JPEG @ %u fps, quality %d",
				 i, st->streams[i].enc_cfg.width,
				 st->streams[i].enc_cfg.height,
				 st->streams[i].enc_cfg.fps_num,
				 st->jpeg_quality);
		} else {
			RSS_INFO("stream%d: %ux%u %s @ %u fps, %u bps",
				 i, st->streams[i].enc_cfg.width,
				 st->streams[i].enc_cfg.height,
				 st->streams[i].enc_cfg.codec == RSS_CODEC_H265 ? "H.265" : "H.264",
				 st->streams[i].enc_cfg.fps_num,
				 st->streams[i].enc_cfg.bitrate);
		}
	}

	/* ── 10. Create SHM rings ── */
	int ring_main_mb = rss_config_get_int(cfg, "ring", "main_data_mb", 4);
	int ring_main_slots = rss_config_get_int(cfg, "ring", "main_slots", 32);
	int ring_sub_mb = rss_config_get_int(cfg, "ring", "sub_data_mb", 1);
	int ring_sub_slots = rss_config_get_int(cfg, "ring", "sub_slots", 32);

	st->streams[0].ring = rss_ring_create("main", ring_main_slots,
					      ring_main_mb * 1024 * 1024);
	if (!st->streams[0].ring) {
		RSS_FATAL("failed to create main ring");
		return RSS_ERR;
	}
	rss_ring_set_stream_info(st->streams[0].ring, 0,
				 st->streams[0].enc_cfg.codec,
				 st->streams[0].enc_cfg.width,
				 st->streams[0].enc_cfg.height,
				 st->streams[0].enc_cfg.fps_num,
				 st->streams[0].enc_cfg.fps_den,
				 rvd_profile_idc(st->streams[0].enc_cfg.profile),
				 rvd_level_idc(st->streams[0].enc_cfg.width,
					       st->streams[0].enc_cfg.height));

	if (st->stream_count > 1 && !st->streams[1].is_jpeg) {
		st->streams[1].ring = rss_ring_create("sub", ring_sub_slots,
						     ring_sub_mb * 1024 * 1024);
		if (!st->streams[1].ring) {
			RSS_FATAL("failed to create sub ring");
			return RSS_ERR;
		}
		rss_ring_set_stream_info(st->streams[1].ring, 1,
					 st->streams[1].enc_cfg.codec,
					 st->streams[1].enc_cfg.width,
					 st->streams[1].enc_cfg.height,
					 st->streams[1].enc_cfg.fps_num,
					 st->streams[1].enc_cfg.fps_den,
					 rvd_profile_idc(st->streams[1].enc_cfg.profile),
					 rvd_level_idc(st->streams[1].enc_cfg.width,
						       st->streams[1].enc_cfg.height));
	}

	/* JPEG ring (small — only needs to hold a few frames) */
	if (st->jpeg_stream >= 0) {
		int ji = st->jpeg_stream;
		st->streams[ji].ring = rss_ring_create("jpeg", 4, 512 * 1024);
		if (!st->streams[ji].ring) {
			RSS_FATAL("failed to create jpeg ring");
			return RSS_ERR;
		}
		rss_ring_set_stream_info(st->streams[ji].ring, 2,
					 RSS_CODEC_JPEG,
					 st->streams[ji].enc_cfg.width,
					 st->streams[ji].enc_cfg.height,
					 st->streams[ji].enc_cfg.fps_num,
					 st->streams[ji].enc_cfg.fps_den,
					 0, 0);
	}

	/* ── 11. OSD consumer init (stub for now) ── */
	rvd_osd_init(st);

	RSS_INFO("pipeline ready: %d streams", st->stream_count);
	return RSS_OK;
}

void rvd_pipeline_deinit(rvd_state_t *st)
{
	rvd_osd_deinit(st);

	for (int i = st->stream_count - 1; i >= 0; i--) {
		if (!st->streams[i].enabled) continue;
		int chn = st->streams[i].chn;

		RSS_HAL_CALL(st->ops, enc_stop, st->hal_ctx, chn);

		if (!st->streams[i].is_jpeg) {
			RSS_HAL_CALL(st->ops, fs_disable_channel, st->hal_ctx, chn);

			rss_cell_t fs_out = { RSS_DEV_FS,  chn, 0 };
			rss_cell_t enc_in = { RSS_DEV_ENC, chn, 0 };
			RSS_HAL_CALL(st->ops, unbind, st->hal_ctx, &fs_out, &enc_in);
		}

		RSS_HAL_CALL(st->ops, enc_unregister_channel, st->hal_ctx, chn);
		RSS_HAL_CALL(st->ops, enc_destroy_channel, st->hal_ctx, chn);
		if (!st->streams[i].is_jpeg) {
			RSS_HAL_CALL(st->ops, enc_destroy_group, st->hal_ctx, chn);
			RSS_HAL_CALL(st->ops, fs_destroy_channel, st->hal_ctx, chn);
		}

		if (st->streams[i].ring) {
			rss_ring_destroy(st->streams[i].ring);
			st->streams[i].ring = NULL;
		}
	}

	if (st->hal_ctx) {
		RSS_HAL_CALL(st->ops, deinit, st->hal_ctx);
		rss_hal_destroy(st->hal_ctx);
		st->hal_ctx = NULL;
	}

	/* Scratch buffers freed in rvd_frame_loop */
}

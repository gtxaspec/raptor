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
	case 0:
		return 66; /* Baseline */
	case 1:
		return 77; /* Main */
	case 2:
		return 100; /* High */
	default:
		return 100;
	}
}

/* Derive H.264 level_idc from resolution (Table A-1 in H.264 spec) */
static uint8_t rvd_level_idc(int width, int height)
{
	int macroblocks = ((width + 15) / 16) * ((height + 15) / 16);
	if (macroblocks <= 99)
		return 10; /* 1.0: up to 176x144 (QCIF) */
	if (macroblocks <= 396)
		return 13; /* 1.3: up to 352x288 (CIF) */
	if (macroblocks <= 792)
		return 21; /* 2.1: up to 480x360 */
	if (macroblocks <= 1620)
		return 30; /* 3.0: up to 720x576 */
	if (macroblocks <= 3600)
		return 31; /* 3.1: up to 1280x720 */
	if (macroblocks <= 5120)
		return 32; /* 3.2: up to 1280x1024 */
	if (macroblocks <= 8192)
		return 40; /* 4.0: up to 1920x1080 */
	if (macroblocks <= 8704)
		return 42; /* 4.2: up to 2048x1088 */
	if (macroblocks <= 22080)
		return 50; /* 5.0: up to 2560x1920 */
	if (macroblocks <= 36864)
		return 51; /* 5.1: up to 4096x2160 */
	return 52;	   /* 5.2 */
}

/* Parse codec string from config */
static rss_codec_t parse_codec(const char *s)
{
	if (!s)
		return RSS_CODEC_H264;
	if (strcasecmp(s, "h265") == 0 || strcasecmp(s, "hevc") == 0)
		return RSS_CODEC_H265;
	if (strcasecmp(s, "jpeg") == 0 || strcasecmp(s, "mjpeg") == 0)
		return RSS_CODEC_JPEG;
	return RSS_CODEC_H264;
}

/* Parse rate control mode string */
static rss_rc_mode_t parse_rc_mode(const char *s)
{
	if (!s)
		return RSS_RC_VBR;
	if (strcasecmp(s, "cbr") == 0)
		return RSS_RC_CBR;
	if (strcasecmp(s, "fixqp") == 0)
		return RSS_RC_FIXQP;
	if (strcasecmp(s, "capped_vbr") == 0)
		return RSS_RC_CAPPED_VBR;
	return RSS_RC_VBR;
}

/* Load one stream's config from INI section */
static void load_stream_config(rss_config_t *cfg, const char *section, rvd_stream_t *s,
			       int default_w, int default_h, int default_fps, int default_br)
{
	int w = rss_config_get_int(cfg, section, "width", default_w);
	int h = rss_config_get_int(cfg, section, "height", default_h);
	int fps = rss_config_get_int(cfg, section, "fps", default_fps);

	/* Framesource config */
	s->fs_cfg = (rss_fs_config_t){
		.width = w,
		.height = h,
		.pixfmt = RSS_PIXFMT_NV12,
		.fps_num = fps,
		.fps_den = 1,
		.nr_vbs = rss_config_get_int(cfg, section, "nr_vbs", 2),
	};

	/* Encoder config */
	s->enc_cfg = (rss_video_config_t){
		.codec = parse_codec(rss_config_get_str(cfg, section, "codec", "h264")),
		.width = w,
		.height = h,
		.profile = rss_config_get_int(cfg, section, "profile", 2),
		.rc_mode = parse_rc_mode(rss_config_get_str(cfg, section, "rc_mode", "vbr")),
		.bitrate = rss_config_get_int(cfg, section, "bitrate", default_br),
		.max_bitrate = rss_config_get_int(cfg, section, "max_bitrate", 0),
		.fps_num = fps,
		.fps_den = 1,
		.gop_length = rss_config_get_int(cfg, section, "gop", fps * 2),
		.init_qp = -1,
		.min_qp = rss_config_get_int(cfg, section, "min_qp", 20),
		.max_qp = rss_config_get_int(cfg, section, "max_qp", 42),
	};
}

/* Load sensor config from an INI section, with procfs fallback for sensor 0 */
static void load_sensor_from_section(rss_config_t *cfg, const char *section,
				     rss_sensor_config_t *sensor, bool use_procfs)
{
	memset(sensor, 0, sizeof(*sensor));

	const char *cfg_name = rss_config_get_str(cfg, section, "name", "");
	if (cfg_name[0]) {
		rss_strlcpy(sensor->name, cfg_name, sizeof(sensor->name));
	} else if (use_procfs) {
		char *s = rss_read_file("/proc/jz/sensor/name", NULL);
		if (s) {
			char *nl = strchr(s, '\n');
			if (nl) *nl = '\0';
			rss_strlcpy(sensor->name, s, sizeof(sensor->name));
			free(s);
		}
	}

	sensor->i2c_addr = rss_config_get_int(cfg, section, "i2c_addr", 0);
	if (sensor->i2c_addr == 0 && use_procfs) {
		char *s = rss_read_file("/proc/jz/sensor/i2c_addr", NULL);
		if (s) { sensor->i2c_addr = (uint16_t)strtol(s, NULL, 0); free(s); }
	}

	sensor->i2c_adapter = rss_config_get_int(cfg, section, "i2c_adapter", -1);
	if (sensor->i2c_adapter < 0 && use_procfs) {
		char *s = rss_read_file("/proc/jz/sensor/i2c_adapter", NULL);
		if (!s) s = rss_read_file("/proc/jz/sensor/i2c_bus", NULL);
		sensor->i2c_adapter = s ? (int)strtol(s, NULL, 10) : 0;
		free(s);
	}
	if (sensor->i2c_adapter < 0)
		sensor->i2c_adapter = 0;

	sensor->sensor_id = rss_config_get_int(cfg, section, "sensor_id", 0);
	sensor->pwdn_gpio = rss_config_get_int(cfg, section, "pwdn_gpio", -1);
	if (sensor->pwdn_gpio == -1 && use_procfs) {
		char *s = rss_read_file("/proc/jz/sensor/pwdn_gpio", NULL);
		if (s) { sensor->pwdn_gpio = (int)strtol(s, NULL, 10); free(s); }
	}
	sensor->power_gpio = rss_config_get_int(cfg, section, "power_gpio", -1);
	sensor->rst_gpio = rss_config_get_int(cfg, section, "rst_gpio", -1);
	if (sensor->rst_gpio == -1 && use_procfs) {
		char *s = rss_read_file("/proc/jz/sensor/rst_gpio", NULL);
		if (s) { sensor->rst_gpio = (int)strtol(s, NULL, 10); free(s); }
	}

	sensor->default_boot = rss_config_get_int(cfg, section, "boot", -1);
	if (sensor->default_boot < 0 && use_procfs) {
		char *s = rss_read_file("/proc/jz/sensor/boot", NULL);
		sensor->default_boot = s ? (int)strtol(s, NULL, 10) : 0;
		free(s);
	}
	if (sensor->default_boot < 0)
		sensor->default_boot = 0;

	sensor->mclk = rss_config_get_int(cfg, section, "mclk", -1);
	if (sensor->mclk < 0 && use_procfs) {
		char *s = rss_read_file("/proc/jz/sensor/mclk", NULL);
		sensor->mclk = s ? (int)strtol(s, NULL, 10) : 0;
		free(s);
	}
	if (sensor->mclk < 0)
		sensor->mclk = 0;

	sensor->vin_type = rss_config_get_int(cfg, section, "video_interface", -1);
	if (sensor->vin_type < 0 && use_procfs) {
		char *s = rss_read_file("/proc/jz/sensor/video_interface", NULL);
		sensor->vin_type = s ? (int)strtol(s, NULL, 10) : 0;
		free(s);
	}
	if (sensor->vin_type < 0)
		sensor->vin_type = 0;
}

/* FS channel base for a given sensor index (hardware mapping) */
static int fs_base_channel(int sensor_idx)
{
	return sensor_idx * 3; /* sensor 0→0, sensor 1→3, sensor 2→6 */
}

/* Ring name for a sensor/stream combination */
static void get_ring_name(int sensor_idx, const char *type, char *buf, size_t len)
{
	if (sensor_idx == 0)
		snprintf(buf, len, "%s", type);
	else
		snprintf(buf, len, "s%d_%s", sensor_idx, type);
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
	rss_multi_sensor_config_t multi_cfg = {0};

	/* Detect multi-sensor: if [sensor1] has a name, use [sensor0]+[sensor1]+... */
	bool multi = (rss_config_get_str(cfg, "sensor1", "name", "")[0] != '\0');
	if (multi) {
		load_sensor_from_section(cfg, "sensor0", &multi_cfg.sensors[0], true);
		load_sensor_from_section(cfg, "sensor1", &multi_cfg.sensors[1], false);
		multi_cfg.sensor_count = 2;
		/* Set sensor_id for secondary sensor */
		if (multi_cfg.sensors[1].sensor_id == 0)
			multi_cfg.sensors[1].sensor_id = 1;

		if (rss_config_get_str(cfg, "sensor2", "name", "")[0] != '\0') {
			load_sensor_from_section(cfg, "sensor2", &multi_cfg.sensors[2], false);
			if (multi_cfg.sensors[2].sensor_id == 0)
				multi_cfg.sensors[2].sensor_id = 2;
			multi_cfg.sensor_count = 3;
		}

		/* MIPI switch config (T23 dual/triple camera) */
		if (rss_config_get_bool(cfg, "mipi_switch", "enabled", false)) {
			multi_cfg.mipi_switch.enable = true;
			multi_cfg.mipi_switch.switch_gpio =
				rss_config_get_int(cfg, "mipi_switch", "switch_gpio", 0);
			multi_cfg.mipi_switch.main_gstate =
				rss_config_get_int(cfg, "mipi_switch", "main_gstate", 0);
			multi_cfg.mipi_switch.sec_gstate =
				rss_config_get_int(cfg, "mipi_switch", "sec_gstate", 1);
			multi_cfg.mipi_switch.switch_gpio2 =
				rss_config_get_int(cfg, "mipi_switch", "switch_gpio2", 0);
		}

		RSS_INFO("multi-sensor mode: %d sensors", multi_cfg.sensor_count);
	} else {
		/* Legacy single-sensor: load from [sensor] section */
		load_sensor_from_section(cfg, "sensor", &multi_cfg.sensors[0], true);
		multi_cfg.sensor_count = 1;
	}
	st->sensor_count = multi_cfg.sensor_count;
	if (st->sensor_count > RVD_MAX_SENSORS) {
		RSS_FATAL("sensor_count %d exceeds RVD_MAX_SENSORS %d", st->sensor_count, RVD_MAX_SENSORS);
		return RSS_ERR;
	}

	/* Validate primary sensor */
	if (!multi_cfg.sensors[0].name[0]) {
		RSS_FATAL("sensor name not in config and not in /proc/jz/sensor/name");
		return RSS_ERR;
	}

	if (multi_cfg.sensors[0].i2c_addr == 0) {
		RSS_FATAL("i2c_addr not in config and not in /proc/jz/sensor/i2c_addr");
		return RSS_ERR;
	}

	for (int s = 0; s < multi_cfg.sensor_count; s++) {
		RSS_DEBUG("sensor%d: %s i2c=0x%02x bus=%d id=%d", s,
			 multi_cfg.sensors[s].name, multi_cfg.sensors[s].i2c_addr,
			 multi_cfg.sensors[s].i2c_adapter, multi_cfg.sensors[s].sensor_id);
	}

	/* ── 3. Init HAL (brings up ISP + sensor(s)) ── */
	ret = RSS_HAL_CALL(st->ops, init, st->hal_ctx, &multi_cfg);
	if (ret != RSS_OK) {
		RSS_FATAL("HAL init failed: %d", ret);
		return ret;
	}

	/* Log system info */
	{
		char ver[64];
		if (rss_hal_get_imp_version(ver, sizeof(ver)) == 0)
			RSS_INFO("LIBIMP Version %s", ver);
		if (rss_hal_get_sysutils_version(ver, sizeof(ver)) == 0)
			RSS_INFO("SYSUTILS Version %s", ver);
		const char *cpu = rss_hal_get_cpu_info();
		if (cpu)
			RSS_INFO("CPU: %s", cpu);
	}

	/* Check caps for codec fallback */
	const rss_hal_caps_t *caps = st->ops->get_caps ? st->ops->get_caps(st->hal_ctx) : NULL;

	/* ── 3b. Set sensor FPS (per sensor for multi-sensor) ── */
	{
		const char *fps_section = multi ? "sensor0" : "sensor";
		int sensor_fps = rss_config_get_int(cfg, fps_section, "fps", 0);
		if (sensor_fps <= 0) {
			char *s = rss_read_file("/proc/jz/sensor/max_fps", NULL);
			if (s) { sensor_fps = (int)strtol(s, NULL, 10); free(s); }
			if (sensor_fps <= 0) sensor_fps = 25;
		}
		/* Sensor 0 FPS via legacy ops */
		ret = RSS_HAL_CALL(st->ops, isp_set_sensor_fps, st->hal_ctx, sensor_fps, 1);
		if (ret != RSS_OK)
			RSS_WARN("isp_set_sensor_fps failed: %d (non-fatal)", ret);
		else
			RSS_INFO("sensor0 fps: %d", sensor_fps);

		/* Additional sensors via _n ops */
		for (int s = 1; s < st->sensor_count; s++) {
			char sect[20];
			snprintf(sect, sizeof(sect), "sensor%d", s);
			int fps = rss_config_get_int(cfg, sect, "fps", sensor_fps);
			RSS_HAL_CALL(st->ops, isp_set_sensor_fps_n, st->hal_ctx, s, fps, 1);
			RSS_DEBUG("sensor%d fps: %d", s, fps);
		}
	}

	/* ── 3c. ISP tuning defaults (apply to all sensors) ── */
	RSS_HAL_CALL(st->ops, isp_set_brightness, st->hal_ctx, 128);
	RSS_HAL_CALL(st->ops, isp_set_contrast, st->hal_ctx, 128);
	RSS_HAL_CALL(st->ops, isp_set_saturation, st->hal_ctx, 128);
	RSS_HAL_CALL(st->ops, isp_set_sharpness, st->hal_ctx, 128);
	RSS_HAL_CALL(st->ops, isp_set_sinter_strength, st->hal_ctx, 128);
	RSS_HAL_CALL(st->ops, isp_set_temper_strength, st->hal_ctx, 128);
	RSS_HAL_CALL(st->ops, isp_set_running_mode, st->hal_ctx, RSS_ISP_DAY);
	ret = RSS_HAL_CALL(st->ops, isp_set_bypass, st->hal_ctx, 1);
	RSS_DEBUG("isp_set_bypass returned %d", ret);
	int antiflicker = rss_config_get_int(cfg, multi ? "sensor0" : "sensor", "antiflicker", 2);
	RSS_HAL_CALL(st->ops, isp_set_antiflicker, st->hal_ctx, antiflicker);
	RSS_HAL_CALL(st->ops, isp_set_hflip, st->hal_ctx, 0);
	RSS_HAL_CALL(st->ops, isp_set_vflip, st->hal_ctx, 0);

	/* Dual-sensor: read sensor attrs + disable AeFreeze + set CustomMode (prudynt pattern).
	 * GetSensorAttr may trigger ISP to initialize the sensor pipeline. */
	if (st->sensor_count > 1) {
		rss_exposure_t dummy;
		RSS_HAL_CALL(st->ops, isp_get_exposure_n, st->hal_ctx, 0, &dummy);
		for (int s = 1; s < st->sensor_count; s++)
			RSS_HAL_CALL(st->ops, isp_get_exposure_n, st->hal_ctx, s, &dummy);
		RSS_HAL_CALL(st->ops, isp_set_ae_freeze_n, st->hal_ctx, 0, 0);
		RSS_HAL_CALL(st->ops, isp_set_custom_mode_n, st->hal_ctx, 0, 0);
	}

	/* Apply defaults to additional sensors */
	for (int s = 1; s < st->sensor_count; s++) {
		RSS_HAL_CALL(st->ops, isp_set_brightness_n, st->hal_ctx, s, 128);
		RSS_HAL_CALL(st->ops, isp_set_contrast_n, st->hal_ctx, s, 128);
		RSS_HAL_CALL(st->ops, isp_set_saturation_n, st->hal_ctx, s, 128);
		RSS_HAL_CALL(st->ops, isp_set_sharpness_n, st->hal_ctx, s, 128);
		RSS_HAL_CALL(st->ops, isp_set_sinter_strength_n, st->hal_ctx, s, 128);
		RSS_HAL_CALL(st->ops, isp_set_temper_strength_n, st->hal_ctx, s, 128);
		RSS_HAL_CALL(st->ops, isp_set_running_mode_n, st->hal_ctx, s, RSS_ISP_DAY);
		RSS_HAL_CALL(st->ops, isp_set_hflip_n, st->hal_ctx, s, 0);
		RSS_HAL_CALL(st->ops, isp_set_vflip_n, st->hal_ctx, s, 0);
		RSS_HAL_CALL(st->ops, isp_set_ae_freeze_n, st->hal_ctx, s, 0);
		RSS_HAL_CALL(st->ops, isp_set_custom_mode_n, st->hal_ctx, s, 0);
	}

	/* ── 3d. Read actual sensor resolution from /proc ── */
	int sensor_w = 0, sensor_h = 0;
	{
		char *s;
		s = rss_read_file("/proc/jz/sensor/width", NULL);
		if (s) {
			sensor_w = (int)strtol(s, NULL, 10);
			free(s);
		}
		s = rss_read_file("/proc/jz/sensor/height", NULL);
		if (s) {
			sensor_h = (int)strtol(s, NULL, 10);
			free(s);
		}
	}
	if (sensor_w > 0 && sensor_h > 0) {
		RSS_INFO("sensor resolution: %dx%d", sensor_w, sensor_h);
	} else {
		/* T40/T41: /proc/jz/sensor doesn't exist — use stream0 config as reference */
		int cfg_w = rss_config_get_int(cfg, "stream0", "width", 0);
		int cfg_h = rss_config_get_int(cfg, "stream0", "height", 0);
		if (cfg_w > 0 && cfg_h > 0) {
			sensor_w = cfg_w;
			sensor_h = cfg_h;
			RSS_INFO("sensor resolution from config: %dx%d", sensor_w, sensor_h);
		} else {
			/* Multi-sensor: procfs may not report resolution with s0/s1 drivers.
			 * Read from sensor0 config section. */
			const char *res_sect = multi ? "sensor0" : "sensor";
			int cfg_sw = rss_config_get_int(cfg, res_sect, "width", 0);
			int cfg_sh = rss_config_get_int(cfg, res_sect, "height", 0);
			if (cfg_sw > 0 && cfg_sh > 0) {
				sensor_w = cfg_sw;
				sensor_h = cfg_sh;
				RSS_INFO("sensor resolution from config: %dx%d", sensor_w, sensor_h);
			} else {
				RSS_WARN("could not determine sensor resolution");
			}
		}
	}

	/* Low latency: encoder releases frames immediately (saves 40-120ms) */
	st->low_latency = rss_config_get_bool(cfg, "sensor", "low_latency", false);
	if (st->low_latency)
		RSS_INFO("low latency mode enabled");

	/* ── 4. Load stream configs (per sensor) ── */
	int def_w = sensor_w > 0 ? sensor_w : 1920;
	int def_h = sensor_h > 0 ? sensor_h : 1080;
	int sub_w = def_w > 640 ? 640 : def_w / 2;
	int sub_h = def_h > 360 ? 360 : def_h / 2;
	st->stream_count = 0;
	int enc_grp_counter = 0; /* sequential encoder group assignment */

	for (int s = 0; s < st->sensor_count; s++) {
		int fs_base = fs_base_channel(s);
		char main_sect[32], sub_sect[32];

		if (s == 0) {
			/* Sensor 0: backward-compatible section names */
			rss_strlcpy(main_sect, "stream0", sizeof(main_sect));
			rss_strlcpy(sub_sect, "stream1", sizeof(sub_sect));
		} else {
			snprintf(main_sect, sizeof(main_sect), "sensor%d_stream0", s);
			snprintf(sub_sect, sizeof(sub_sect), "sensor%d_stream1", s);
		}

		/* Main stream */
		int si = st->stream_count;
		load_stream_config(cfg, main_sect, &st->streams[si], def_w, def_h, 25, 2500000);
		st->streams[si].enc_cfg.ivdc = rss_config_get_bool(cfg, main_sect, "ivdc", false);
		st->streams[si].fs_chn = fs_base;
		st->streams[si].chn = enc_grp_counter++;
		st->streams[si].sensor_idx = s;
		rss_strlcpy(st->streams[si].cfg_sect, main_sect, sizeof(st->streams[si].cfg_sect));
		st->stream_count++;

		RSS_DEBUG("sensor%d main: fs_chn=%d enc_grp=%d %ux%u", s,
			 st->streams[si].fs_chn, st->streams[si].chn,
			 st->streams[si].enc_cfg.width, st->streams[si].enc_cfg.height);

		/* Sub stream (optional) */
		bool sub_enabled = (s == 0) ? rss_config_get_bool(cfg, "stream1", "enabled", true)
					    : rss_config_get_bool(cfg, sub_sect, "enabled", true);
		if (sub_enabled) {
			si = st->stream_count;
			load_stream_config(cfg, sub_sect, &st->streams[si], sub_w, sub_h, 25, 1000000);
			st->streams[si].fs_chn = fs_base + 1;
			st->streams[si].chn = enc_grp_counter++;
			st->streams[si].sensor_idx = s;
			rss_strlcpy(st->streams[si].cfg_sect, sub_sect, sizeof(st->streams[si].cfg_sect));
			st->stream_count++;

			RSS_DEBUG("sensor%d sub: fs_chn=%d enc_grp=%d %ux%u", s,
				 st->streams[si].fs_chn, st->streams[si].chn,
				 st->streams[si].enc_cfg.width, st->streams[si].enc_cfg.height);
		}
	}

	/* IVDC encoder group reorder: SDK requires IVDC channels in
	 * groups 0..N-1. Reassign groups so IVDC mains come first. */
	{
		int ivdc_count = 0;
		for (int i = 0; i < st->stream_count; i++)
			if (st->streams[i].enc_cfg.ivdc)
				ivdc_count++;

		if (ivdc_count > 0) {
			int grp_ivdc = 0, grp_other = ivdc_count;
			for (int i = 0; i < st->stream_count; i++) {
				int old_grp = st->streams[i].chn;
				if (st->streams[i].enc_cfg.ivdc)
					st->streams[i].chn = grp_ivdc++;
				else
					st->streams[i].chn = grp_other++;
				if (st->streams[i].chn != old_grp)
					RSS_DEBUG("ivdc reorder: stream%d enc_grp %d -> %d",
						  i, old_grp, st->streams[i].chn);
			}
			enc_grp_counter = grp_other;
		}
	}

	/* JPEG snapshot channels (one per video stream) */
	int jpeg_chn_base = enc_grp_counter;
	st->jpeg_count = 0;
	for (int j = 0; j < RVD_MAX_JPEG; j++)
		st->jpeg_streams[j] = -1;

	if (rss_config_get_bool(cfg, "jpeg", "enabled", true)) {
		int def_quality = rss_config_get_int(cfg, "jpeg", "quality", 75);
		int def_fps = rss_config_get_int(cfg, "jpeg", "fps", 1);
		int video_count = st->stream_count;

		for (int v = 0; v < video_count && st->jpeg_count < RVD_MAX_JPEG; v++) {
			/* IVDC streams: JPEG+IVDC registration fails on T23 dual-sensor
			 * (SDK rejects second IVDC channel in same group). Skip JPEG
			 * but keep index for consistent ring naming. */
			if (st->streams[v].enc_cfg.ivdc) {
				st->jpeg_count++;
				continue;
			}
			const char *sect = st->streams[v].cfg_sect;
			if (!rss_config_get_bool(cfg, sect, "jpeg", true))
				continue;

			int quality = rss_config_get_int(cfg, sect, "jpeg_quality", def_quality);
			if (quality < 1) quality = 1;
			if (quality > 100) quality = 100;
			int fps = rss_config_get_int(cfg, sect, "jpeg_fps", def_fps);
			if (fps < 1) fps = 1;

			int ji = st->stream_count;
			int jpeg_chn = jpeg_chn_base + st->jpeg_count;

			st->streams[ji].enc_cfg = (rss_video_config_t){
				.codec = RSS_CODEC_JPEG,
				.width = st->streams[v].enc_cfg.width,
				.height = st->streams[v].enc_cfg.height,
				.fps_num = fps,
				.fps_den = 1,
				.bitrate = 0,
			};
			st->streams[ji].fs_cfg = st->streams[v].fs_cfg;
			st->streams[ji].fs_chn = st->streams[v].fs_chn;
			st->streams[ji].chn = jpeg_chn;
			st->streams[ji].sensor_idx = st->streams[v].sensor_idx;
			st->streams[ji].is_jpeg = true;
			st->streams[ji].enc_cfg.init_qp = quality;
			st->jpeg_streams[st->jpeg_count] = ji;
			st->stream_count = ji + 1;
			st->jpeg_count++;

			RSS_INFO("jpeg%d: [%s] sensor%d %ux%u @ %d fps, quality %d (enc chn %d)",
				 st->jpeg_count - 1, sect, st->streams[ji].sensor_idx,
				 st->streams[ji].enc_cfg.width, st->streams[ji].enc_cfg.height,
				 fps, quality, jpeg_chn);
		}
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

	/* ── 4b. Enable crop + scaler for streams ── */
	if (sensor_w > 0 && sensor_h > 0) {
		for (int i = 0; i < st->stream_count; i++) {
			if (st->streams[i].is_jpeg)
				continue;
			rss_fs_config_t *fs = &st->streams[i].fs_cfg;

			/* In multi-sensor mode, ISP reports sensor resolution as 0x0 at
			 * framesource creation time. Enable crop on main streams (full res)
			 * to explicitly set the input resolution. Skip on sub streams
			 * since crop dimensions can't exceed scaler output. */
			if (st->sensor_count > 1 &&
			    fs->width == sensor_w && fs->height == sensor_h) {
				fs->crop.enable = true;
				fs->crop.x = 0;
				fs->crop.y = 0;
				fs->crop.w = sensor_w;
				fs->crop.h = sensor_h;
			}

			if (fs->width != sensor_w || fs->height != sensor_h) {
				fs->scaler.enable = true;
				fs->scaler.out_width = fs->width;
				fs->scaler.out_height = fs->height;
				RSS_DEBUG("stream%d: scaler %dx%d -> %dx%d", i, sensor_w, sensor_h,
					 fs->width, fs->height);
			}
		}
	}

	/* ── 5. Create framesource channels ── */
	/* JPEG shares framesource with its video stream — skip FS creation for it */
	for (int i = 0; i < st->stream_count; i++) {
		if (st->streams[i].is_jpeg)
			continue;
		int fsc = st->streams[i].fs_chn;
		ret = RSS_HAL_CALL(st->ops, fs_create_channel, st->hal_ctx, fsc,
				   &st->streams[i].fs_cfg);
		if (ret != RSS_OK) {
			RSS_FATAL("fs_create_channel(%d) failed: %d", fsc, ret);
			return ret;
		}
		RSS_HAL_CALL(st->ops, fs_set_fifo, st->hal_ctx, fsc, 0);
		RSS_HAL_CALL(st->ops, fs_set_frame_depth, st->hal_ctx, fsc, 0);
	}

	/* ── 6. OSD pipeline (if enabled) ── */
	st->osd_enabled = rss_config_get_bool(cfg, "osd", "enabled", true);
	st->use_isp_osd = rss_config_get_bool(cfg, "osd", "isp_osd", false);
	if (st->use_isp_osd && (!caps || !caps->has_isp_osd)) {
		RSS_WARN("isp_osd requested but not supported on this platform, using IPU OSD");
		st->use_isp_osd = false;
	}
	if (st->osd_enabled && st->use_isp_osd) {
		/* Hybrid: ISP OSD for mains, IPU OSD for subs */
		RSS_HAL_CALL(st->ops, isp_osd_set_pool_size, st->hal_ctx, 512 * 1024);
		RSS_HAL_CALL(st->ops, osd_set_pool_size, st->hal_ctx, 512 * 1024);

		for (int i = 0; i < st->stream_count; i++) {
			if (st->streams[i].is_jpeg)
				continue;
			if (st->streams[i].fs_chn % 3 == 0)
				continue; /* main — uses ISP OSD, no group */
			int grp = st->streams[i].chn;
			ret = RSS_HAL_CALL(st->ops, osd_create_group, st->hal_ctx, grp);
			if (ret != RSS_OK) {
				RSS_WARN("osd_create_group(%d) failed: %d", grp, ret);
				continue;
			}
			RSS_DEBUG("osd group %d created (sub, IPU OSD)", grp);
		}
		RSS_INFO("using hybrid OSD: ISP for mains, IPU for subs");
	} else if (st->osd_enabled) {
		/* IPU OSD only: create groups for all streams */
		RSS_HAL_CALL(st->ops, osd_set_pool_size, st->hal_ctx, 512 * 1024);

		for (int i = 0; i < st->stream_count; i++) {
			if (st->streams[i].is_jpeg)
				continue;
			int grp = st->streams[i].chn;
			ret = RSS_HAL_CALL(st->ops, osd_create_group, st->hal_ctx, grp);
			if (ret != RSS_OK) {
				RSS_WARN("osd_create_group(%d) failed: %d, disabling OSD", grp,
					 ret);
				st->osd_enabled = false;
				break;
			}
			RSS_DEBUG("osd group %d created", grp);
		}
	}

	/* ── 6b. Create OSD regions + Start (before bind, per vendor sample) ── */
	rvd_osd_init(st);
	/* IPU OSD: OSD_Start BEFORE bind, matching vendor SDK sample sequence:
	 * CreateGroup → CreateRgn → Register → SetAttr → SetGrpRgnAttr → Start → Bind
	 * ISP OSD: no start needed — regions are shown via ShowOsdRgn */
	if (st->osd_enabled) {
		for (int i = 0; i < st->stream_count; i++) {
			if (st->streams[i].is_jpeg)
				continue;
			/* ISP OSD mains don't need osd_start */
			if (st->use_isp_osd && st->streams[i].fs_chn % 3 == 0)
				continue;
			int grp = st->streams[i].chn;
			ret = RSS_HAL_CALL(st->ops, osd_start, st->hal_ctx, grp);
			if (ret != RSS_OK)
				RSS_WARN("osd_start(%d) failed: %d", grp, ret);
			else
				RSS_DEBUG("osd_start(%d) ok", grp);
		}
	}

	/* ── 7. Create encoder groups and channels ── */
	/* Video streams: create group, create chn, register chn in group.
	 * JPEG: no group (registers into group 0), uses enc chn 4+ per SDK convention. */
	for (int i = 0; i < st->stream_count; i++) {
		int chn = st->streams[i].chn;

		if (st->streams[i].is_jpeg) {
			/* Find parent video stream's encoder group */
			int video_grp = 0;
			for (int v = 0; v < st->stream_count; v++) {
				if (!st->streams[v].is_jpeg &&
				    st->streams[v].fs_chn == st->streams[i].fs_chn) {
					video_grp = st->streams[v].chn;
					break;
				}
			}

			/* Optional buffer sharing (before CreateChn) */
			if (rss_config_get_bool(cfg, "jpeg", "bufshare", true)) {
				ret = RSS_HAL_CALL(st->ops, enc_set_bufshare, st->hal_ctx, chn,
						   video_grp);
				if (ret == RSS_OK)
					RSS_DEBUG("jpeg bufshare chn %d -> group %d", chn,
						  video_grp);
				else
					RSS_WARN("jpeg bufshare failed: %d (non-fatal)", ret);
			}

			ret = RSS_HAL_CALL(st->ops, enc_create_channel, st->hal_ctx, chn,
					   &st->streams[i].enc_cfg);
			if (ret != RSS_OK) {
				RSS_FATAL("enc_create_channel(%d/JPEG) failed: %d", chn, ret);
				return ret;
			}

			/* Register JPEG into corresponding video group */
			ret = RSS_HAL_CALL(st->ops, enc_register_channel, st->hal_ctx, video_grp,
					   chn);
			if (ret != RSS_OK) {
				RSS_FATAL("enc_register_channel(%d, %d) failed: %d", video_grp, chn,
					  ret);
				return ret;
			}
		} else {
			ret = RSS_HAL_CALL(st->ops, enc_create_group, st->hal_ctx, chn);
			if (ret != RSS_OK) {
				RSS_FATAL("enc_create_group(%d) failed: %d", chn, ret);
				return ret;
			}

			ret = RSS_HAL_CALL(st->ops, enc_create_channel, st->hal_ctx, chn,
					   &st->streams[i].enc_cfg);
			if (ret != RSS_OK) {
				RSS_FATAL("enc_create_channel(%d) failed: %d", chn, ret);
				return ret;
			}

			/* Low latency: encoder releases each frame immediately
			 * instead of batching (saves 1-3 frame periods = 40-120ms) */
			if (st->low_latency)
				RSS_HAL_CALL(st->ops, enc_set_max_stream_cnt, st->hal_ctx, chn, 1);

			ret = RSS_HAL_CALL(st->ops, enc_register_channel, st->hal_ctx, chn, chn);
			if (ret != RSS_OK) {
				RSS_FATAL("enc_register_channel(%d) failed: %d", chn, ret);
				return ret;
			}
		}
	}

	/* ── 8a. IVS init (before bind — SDK requires all groups created before FS enable) ── */
	st->ivs_enabled = rss_config_get_bool(cfg, "motion", "enabled", false);
	if (st->ivs_enabled) {
		int ivs_ret = rvd_ivs_init(st);
		if (ivs_ret != RSS_OK) {
			RSS_WARN("IVS init failed: %d (motion detection disabled)", ivs_ret);
			st->ivs_enabled = false;
		}
	}

	/* ── 8b. Bind pipeline ── */
	/* Build a chain of cells for each video stream and bind them sequentially.
	 * JPEG channels share a framesource — no separate bind needed.
	 * Chain varies by stream: FS [→ IVS] [→ OSD] → ENC */
	for (int i = 0; i < st->stream_count; i++) {
		if (st->streams[i].is_jpeg)
			continue;
		int fsc = st->streams[i].fs_chn;
		int grp = st->streams[i].chn;
		bool insert_ivs = (fsc == 1 && st->ivs_active);

		rss_cell_t chain[RVD_MAX_BIND_STAGES];
		int chain_len = 0;

		chain[chain_len++] = (rss_cell_t){RSS_DEV_FS, fsc, 0};
		if (insert_ivs)
			chain[chain_len++] = (rss_cell_t){RSS_DEV_IVS, 0, 0};
		/* OSD in bind chain: always for IPU-only mode, subs only for hybrid */
		if (st->osd_enabled &&
		    (!st->use_isp_osd || st->streams[i].fs_chn % 3 != 0))
			chain[chain_len++] = (rss_cell_t){RSS_DEV_OSD, grp, 0};
		chain[chain_len++] = (rss_cell_t){RSS_DEV_ENC, grp, 0};

		for (int j = 0; j < chain_len - 1; j++) {
			ret = RSS_HAL_CALL(st->ops, bind, st->hal_ctx, &chain[j], &chain[j + 1]);
			if (ret != RSS_OK) {
				RSS_FATAL("bind failed at step %d for stream %d: %d", j, i, ret);
				return ret;
			}
		}

		/* Store chain for unbind at deinit */
		memcpy(st->bind_chain[i], chain, sizeof(rss_cell_t) * chain_len);
		st->bind_chain_len[i] = chain_len;

		RSS_DEBUG("stream%d bind: %d stages", i, chain_len);
	}

	/* IVS: create algo interface + channel + register BEFORE FS enable.
	 * The SDK aborts if frames arrive at an IVS group with no channel. */
	if (st->ivs_active) {
		int ivs_ret = rvd_ivs_start(st);
		if (ivs_ret != RSS_OK) {
			RSS_WARN("IVS start failed: %d (motion detection disabled)", ivs_ret);
		}
	}

	/* ── 9. Enable framesource, start encoder ── */
	for (int i = 0; i < st->stream_count; i++) {
		int fsc = st->streams[i].fs_chn;
		int chn = st->streams[i].chn;

		/* JPEG shares framesource — don't enable twice */
		if (!st->streams[i].is_jpeg) {
			ret = RSS_HAL_CALL(st->ops, fs_enable_channel, st->hal_ctx, fsc);
			if (ret != RSS_OK) {
				RSS_FATAL("fs_enable_channel(%d) failed: %d", fsc, ret);
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
			RSS_INFO("stream%d: %ux%u JPEG @ %u fps, quality %d", i,
				 st->streams[i].enc_cfg.width, st->streams[i].enc_cfg.height,
				 st->streams[i].enc_cfg.fps_num, st->streams[i].enc_cfg.init_qp);
		} else {
			RSS_INFO("stream%d: %ux%u %s @ %u fps, %u bps", i,
				 st->streams[i].enc_cfg.width, st->streams[i].enc_cfg.height,
				 st->streams[i].enc_cfg.codec == RSS_CODEC_H265 ? "H.265" : "H.264",
				 st->streams[i].enc_cfg.fps_num, st->streams[i].enc_cfg.bitrate);
		}
	}

	/* ── 10. Create SHM rings ── */

	/* Auto-size ring data region from bitrate and slot count.
	 * max_frame ≈ bitrate / fps / 8 * 4 (4× headroom for I-frames).
	 * ring_data = max_frame * slots, clamped to [256KB .. config max].
	 * Config value of 0 = auto (recommended). Non-zero = explicit MB. */
	int ring_main_slots = rss_config_get_int(cfg, "ring", "main_slots", 32);
	int ring_sub_slots = rss_config_get_int(cfg, "ring", "sub_slots", 32);
	int ring_main_cfg_mb = rss_config_get_int(cfg, "ring", "main_data_mb", 0);
	int ring_sub_cfg_mb = rss_config_get_int(cfg, "ring", "sub_data_mb", 0);

	uint32_t main_data;
	if (ring_main_cfg_mb > 0) {
		main_data = (uint32_t)ring_main_cfg_mb * 1024 * 1024;
	} else {
		uint32_t bps = st->streams[0].enc_cfg.bitrate;
		uint32_t fps = st->streams[0].enc_cfg.fps_num;
		if (fps == 0)
			fps = 25;
		uint32_t max_frame = (uint32_t)((uint64_t)bps * 4 / 8 / fps);
		if (max_frame < 8192)
			max_frame = 8192;
		main_data = max_frame * (uint32_t)ring_main_slots;
		if (main_data < 256 * 1024)
			main_data = 256 * 1024;
		if (main_data > 8 * 1024 * 1024)
			main_data = 8 * 1024 * 1024;
	}
	RSS_DEBUG("main ring: %u slots, %u KB data", ring_main_slots, main_data / 1024);

	/* Create video rings for all non-JPEG streams */
	for (int i = 0; i < st->stream_count; i++) {
		if (st->streams[i].is_jpeg)
			continue;

		/* Determine ring name based on sensor and local stream index */
		int local = st->streams[i].fs_chn - fs_base_channel(st->streams[i].sensor_idx);
		const char *type = (local == 0) ? "main" : "sub";
		char ring_name[24];
		get_ring_name(st->streams[i].sensor_idx, type, ring_name, sizeof(ring_name));

		/* Auto-size ring from bitrate */
		bool is_main = (local == 0);
		int slots_cfg = is_main ? ring_main_slots : ring_sub_slots;
		int mb_cfg = is_main ? ring_main_cfg_mb : ring_sub_cfg_mb;
		uint32_t min_data = is_main ? (256 * 1024) : (128 * 1024);
		uint32_t max_data = is_main ? (8 * 1024 * 1024) : (4 * 1024 * 1024);
		uint32_t min_frame = is_main ? 8192 : 4096;

		uint32_t data;
		if (mb_cfg > 0) {
			data = (uint32_t)mb_cfg * 1024 * 1024;
		} else {
			uint32_t bps = st->streams[i].enc_cfg.bitrate;
			uint32_t fps = st->streams[i].enc_cfg.fps_num;
			if (fps == 0) fps = 25;
			uint32_t max_frame = (uint32_t)((uint64_t)bps * 4 / 8 / fps);
			if (max_frame < min_frame) max_frame = min_frame;
			data = max_frame * (uint32_t)slots_cfg;
			if (data < min_data) data = min_data;
			if (data > max_data) data = max_data;
		}
		RSS_DEBUG("%s ring: %u slots, %u KB data", ring_name, slots_cfg, data / 1024);

		st->streams[i].ring = rss_ring_create(ring_name, slots_cfg, data);
		if (!st->streams[i].ring) {
			RSS_FATAL("failed to create %s ring", ring_name);
			return RSS_ERR;
		}
		rss_ring_set_stream_info(
			st->streams[i].ring, i, st->streams[i].enc_cfg.codec,
			st->streams[i].enc_cfg.width, st->streams[i].enc_cfg.height,
			st->streams[i].enc_cfg.fps_num, st->streams[i].enc_cfg.fps_den,
			rvd_profile_idc(st->streams[i].enc_cfg.profile),
			rvd_level_idc(st->streams[i].enc_cfg.width, st->streams[i].enc_cfg.height));
	}

	/* JPEG rings — auto-sized from resolution */
	for (int j = 0; j < st->jpeg_count; j++) {
		int ji = st->jpeg_streams[j];
		if (ji < 0)
			continue;
		char ring_name[24];
		char base_name[16];
		snprintf(base_name, sizeof(base_name), "jpeg%d", j);
		get_ring_name(st->streams[ji].sensor_idx, base_name, ring_name, sizeof(ring_name));
		/* Estimate per-frame JPEG size with quality scaling:
		 *   Measured: 1440p q75 ≈ 76KB, 360p q75 ≈ 6KB
		 *   Conservative estimate with ~3x headroom over measured.
		 *   Slots scale with fps (must be power-of-2):
		 *     1-2 fps → 4, 3-8 fps → 8, 9+ fps → 16 */
		uint32_t w = st->streams[ji].enc_cfg.width;
		uint32_t h = st->streams[ji].enc_cfg.height;
		uint32_t q = (uint32_t)st->streams[ji].enc_cfg.init_qp;
		uint32_t fps = st->streams[ji].enc_cfg.fps_num;
		uint32_t divisor = (q >= 90) ? 6 : (q >= 70) ? 16 : 24;
		uint32_t slots = (fps >= 9) ? 16 : (fps >= 3) ? 8 : 4;
		uint32_t jpeg_max = w * h / divisor;
		if (jpeg_max < 16384)
			jpeg_max = 16384;
		uint32_t jpeg_data = jpeg_max * slots;
		if (jpeg_data > 4 * 1024 * 1024)
			jpeg_data = 4 * 1024 * 1024;
		RSS_DEBUG("%s ring: %u slots, %u KB data (q%u, /%u, %u fps)",
			 ring_name, slots, jpeg_data / 1024, q, divisor, fps);

		st->streams[ji].ring = rss_ring_create(ring_name, slots, jpeg_data);
		if (!st->streams[ji].ring) {
			RSS_FATAL("failed to create %s ring", ring_name);
			return RSS_ERR;
		}
		rss_ring_set_stream_info(
			st->streams[ji].ring, 0x20 + j, RSS_CODEC_JPEG,
			st->streams[ji].enc_cfg.width, st->streams[ji].enc_cfg.height,
			st->streams[ji].enc_cfg.fps_num, st->streams[ji].enc_cfg.fps_den, 0, 0);
	}

	/* OSD regions already created in step 6b */

	st->pipeline_ready = true;
	RSS_INFO("pipeline ready: %d streams", st->stream_count);
	return RSS_OK;
}

void rvd_pipeline_deinit(rvd_state_t *st)
{
	/* IVS stop: StopRecvPic + UnRegister + DestroyChn (before FS disable) */
	if (st->ivs_active)
		rvd_ivs_stop(st);

	rvd_osd_deinit(st);

	for (int i = st->stream_count - 1; i >= 0; i--) {
		if (!st->streams[i].enabled)
			continue;
		int chn = st->streams[i].chn;
		int fsc = st->streams[i].fs_chn;

		RSS_HAL_CALL(st->ops, enc_stop, st->hal_ctx, chn);

		if (!st->streams[i].is_jpeg) {
			RSS_HAL_CALL(st->ops, fs_disable_channel, st->hal_ctx, fsc);

			/* Unbind: walk the stored chain in reverse */
			int len = st->bind_chain_len[i];
			for (int j = len - 1; j > 0; j--)
				RSS_HAL_CALL(st->ops, unbind, st->hal_ctx,
					     &st->bind_chain[i][j - 1], &st->bind_chain[i][j]);
		}

		RSS_HAL_CALL(st->ops, enc_unregister_channel, st->hal_ctx, chn);
		RSS_HAL_CALL(st->ops, enc_destroy_channel, st->hal_ctx, chn);
		if (!st->streams[i].is_jpeg) {
			RSS_HAL_CALL(st->ops, enc_destroy_group, st->hal_ctx, chn);
			if (st->osd_enabled &&
			    (!st->use_isp_osd || st->streams[i].fs_chn % 3 != 0))
				RSS_HAL_CALL(st->ops, osd_destroy_group, st->hal_ctx, chn);
			RSS_HAL_CALL(st->ops, fs_destroy_channel, st->hal_ctx, fsc);
		}

		if (st->streams[i].ring) {
			rss_ring_destroy(st->streams[i].ring);
			st->streams[i].ring = NULL;
		}
	}

	/* IVS group destroy — after unbind (SDK requirement) */
	if (st->ivs_active)
		rvd_ivs_deinit(st);

	if (st->hal_ctx) {
		RSS_HAL_CALL(st->ops, deinit, st->hal_ctx);
		rss_hal_destroy(st->hal_ctx);
		st->hal_ctx = NULL;
	}

	/* Scratch buffers freed in rvd_frame_loop */
}

/*
 * rvd_ctrl.c -- Control socket command handler
 *
 * Handles raptorctl commands for encoder settings, ISP tuning,
 * config management, privacy mode, and IVS status.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <stdatomic.h>

#include "rvd.h"

/* ── JSON helpers ── */

int rvd_json_get_int(const char *json, const char *key, int *out)
{
	char pattern[64];
	snprintf(pattern, sizeof(pattern), "\"%s\":", key);
	const char *p = strstr(json, pattern);
	if (!p)
		return -1;
	p += strlen(pattern);
	while (*p == ' ')
		p++;
	*out = (int)strtol(p, NULL, 10);
	return 0;
}

int rvd_json_get_str(const char *json, const char *key, char *buf, int bufsz)
{
	char pattern[64];
	snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
	const char *p = strstr(json, pattern);
	if (!p)
		return -1;
	p += strlen(pattern);
	const char *end = strchr(p, '"');
	if (!end)
		return -1;
	int len = (int)(end - p);
	if (len >= bufsz)
		len = bufsz - 1;
	memcpy(buf, p, len);
	buf[len] = '\0';
	return 0;
}

/* ── Helpers ── */

/*
 * Control handler return convention: return value = response length.
 * The IPC layer uses it as the number of bytes to send back.
 */
#define CTRL_RETURN(buf) return (int)strlen(buf)

static const char *stream_section(int idx)
{
	return (idx >= 0 && idx <= 1) ? (idx == 0 ? "stream0" : "stream1") : "stream0";
}

/* ── Encoder commands ── */

static int handle_encoder_cmd(const char *cmd_json, rvd_state_t *st, char *resp, int resp_size)
{
	int chn, val, val2;

	if (strstr(cmd_json, "\"request-idr\"")) {
		int target = -1;
		rvd_json_get_int(cmd_json, "channel", &target);
		for (int i = 0; i < st->stream_count; i++) {
			if (target >= 0 && i != target)
				continue;
			RSS_HAL_CALL(st->ops, enc_request_idr, st->hal_ctx, st->streams[i].chn);
		}
		snprintf(resp, resp_size, "{\"status\":\"ok\"}");
		return 1;
	}

	if (strstr(cmd_json, "\"set-bitrate\"")) {
		if (rvd_json_get_int(cmd_json, "channel", &chn) == 0 &&
		    rvd_json_get_int(cmd_json, "value", &val) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			int ret = RSS_HAL_CALL(st->ops, enc_set_bitrate, st->hal_ctx,
					       st->streams[chn].chn, val);
			if (ret == 0) {
				st->streams[chn].enc_cfg.bitrate = val;
				rss_config_set_int(st->cfg, stream_section(chn), "bitrate", val);
			}
			snprintf(resp, resp_size, "{\"status\":\"%s\"}", ret == 0 ? "ok" : "error");
		} else {
			snprintf(resp, resp_size,
				 "{\"status\":\"error\",\"reason\":\"need channel and value\"}");
		}
		return 1;
	}

	if (strstr(cmd_json, "\"set-gop\"")) {
		if (rvd_json_get_int(cmd_json, "channel", &chn) == 0 &&
		    rvd_json_get_int(cmd_json, "value", &val) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			int ret = RSS_HAL_CALL(st->ops, enc_set_gop, st->hal_ctx,
					       st->streams[chn].chn, val);
			if (ret == 0) {
				st->streams[chn].enc_cfg.gop_length = val;
				rss_config_set_int(st->cfg, stream_section(chn), "gop", val);
			}
			snprintf(resp, resp_size, "{\"status\":\"%s\"}", ret == 0 ? "ok" : "error");
		} else {
			snprintf(resp, resp_size,
				 "{\"status\":\"error\",\"reason\":\"need channel and value\"}");
		}
		return 1;
	}

	if (strstr(cmd_json, "\"set-fps\"")) {
		if (rvd_json_get_int(cmd_json, "channel", &chn) == 0 &&
		    rvd_json_get_int(cmd_json, "value", &val) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			int ret = RSS_HAL_CALL(st->ops, enc_set_fps, st->hal_ctx,
					       st->streams[chn].chn, val, 1);
			if (ret == 0) {
				st->streams[chn].enc_cfg.fps_num = val;
				rss_config_set_int(st->cfg, stream_section(chn), "fps", val);
			}
			snprintf(resp, resp_size, "{\"status\":\"%s\"}", ret == 0 ? "ok" : "error");
		} else {
			snprintf(resp, resp_size,
				 "{\"status\":\"error\",\"reason\":\"need channel and value\"}");
		}
		return 1;
	}

	if (strstr(cmd_json, "\"set-qp-bounds\"")) {
		if (rvd_json_get_int(cmd_json, "channel", &chn) == 0 &&
		    rvd_json_get_int(cmd_json, "min", &val) == 0 &&
		    rvd_json_get_int(cmd_json, "max", &val2) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			int ret = RSS_HAL_CALL(st->ops, enc_set_qp_bounds, st->hal_ctx,
					       st->streams[chn].chn, val, val2);
			if (ret == 0) {
				st->streams[chn].enc_cfg.min_qp = val;
				st->streams[chn].enc_cfg.max_qp = val2;
				rss_config_set_int(st->cfg, stream_section(chn), "min_qp", val);
				rss_config_set_int(st->cfg, stream_section(chn), "max_qp", val2);
			}
			snprintf(resp, resp_size, "{\"status\":\"%s\"}", ret == 0 ? "ok" : "error");
		} else {
			snprintf(resp, resp_size,
				 "{\"status\":\"error\",\"reason\":\"need channel, min, max\"}");
		}
		return 1;
	}

	return 0; /* not an encoder command */
}

/* ── ISP commands ── */

static int handle_isp_cmd(const char *cmd_json, rvd_state_t *st, char *resp, int resp_size)
{
	int val;

#define ISP_SET(name, fn)                                                                          \
	if (strstr(cmd_json, "\"" name "\"")) {                                                    \
		if (rvd_json_get_int(cmd_json, "value", &val) == 0) {                              \
			int ret = RSS_HAL_CALL(st->ops, fn, st->hal_ctx, val);                     \
			snprintf(resp, resp_size, "{\"status\":\"%s\"}",                            \
				 ret == 0 ? "ok" : "error");                                       \
		} else {                                                                           \
			snprintf(resp, resp_size,                                                  \
				 "{\"status\":\"error\",\"reason\":\"need value\"}");              \
		}                                                                                  \
		return 1;                                                                          \
	}

	ISP_SET("set-brightness", isp_set_brightness)
	ISP_SET("set-contrast", isp_set_contrast)
	ISP_SET("set-saturation", isp_set_saturation)
	ISP_SET("set-sharpness", isp_set_sharpness)
	ISP_SET("set-hue", isp_set_hue)
	ISP_SET("set-sinter", isp_set_sinter_strength)
	ISP_SET("set-temper", isp_set_temper_strength)
	ISP_SET("set-dpc", isp_set_dpc_strength)
	ISP_SET("set-drc", isp_set_drc_strength)
	ISP_SET("set-highlight-depress", isp_set_highlight_depress)
	ISP_SET("set-ae-comp", isp_set_ae_comp)
	ISP_SET("set-max-again", isp_set_max_again)
	ISP_SET("set-max-dgain", isp_set_max_dgain)
	ISP_SET("set-hflip", isp_set_hflip)
	ISP_SET("set-vflip", isp_set_vflip)
	ISP_SET("set-defog", isp_set_defog)
	ISP_SET("set-antiflicker", isp_set_antiflicker)

#undef ISP_SET

	if (strstr(cmd_json, "\"get-isp\"")) {
		uint8_t bri = 0, con = 0, sat = 0, shp = 0, hue = 0, sin = 0, tem = 0;
		int hf = 0, vf = 0, ae = 0;
		uint32_t again = 0, dgain = 0;
		RSS_HAL_CALL(st->ops, isp_get_brightness, st->hal_ctx, &bri);
		RSS_HAL_CALL(st->ops, isp_get_contrast, st->hal_ctx, &con);
		RSS_HAL_CALL(st->ops, isp_get_saturation, st->hal_ctx, &sat);
		RSS_HAL_CALL(st->ops, isp_get_sharpness, st->hal_ctx, &shp);
		RSS_HAL_CALL(st->ops, isp_get_hue, st->hal_ctx, &hue);
		RSS_HAL_CALL(st->ops, isp_get_sinter_strength, st->hal_ctx, &sin);
		RSS_HAL_CALL(st->ops, isp_get_temper_strength, st->hal_ctx, &tem);
		RSS_HAL_CALL(st->ops, isp_get_hvflip, st->hal_ctx, &hf, &vf);
		RSS_HAL_CALL(st->ops, isp_get_ae_comp, st->hal_ctx, &ae);
		RSS_HAL_CALL(st->ops, isp_get_max_again, st->hal_ctx, &again);
		RSS_HAL_CALL(st->ops, isp_get_max_dgain, st->hal_ctx, &dgain);
		snprintf(resp, resp_size,
			 "{\"status\":\"ok\","
			 "\"brightness\":%u,\"contrast\":%u,\"saturation\":%u,"
			 "\"sharpness\":%u,\"hue\":%u,\"sinter\":%u,\"temper\":%u,"
			 "\"hflip\":%d,\"vflip\":%d,\"ae_comp\":%d,"
			 "\"max_again\":%u,\"max_dgain\":%u}",
			 bri, con, sat, shp, hue, sin, tem, hf, vf, ae, again, dgain);
		return 1;
	}

	if (strstr(cmd_json, "\"get-exposure\"")) {
		rss_exposure_t exp = {0};
		RSS_HAL_CALL(st->ops, isp_get_exposure, st->hal_ctx, &exp);
		snprintf(resp, resp_size, "{\"total_gain\":%u,\"exposure_us\":%u,\"ae_luma\":%u}",
			 exp.total_gain, exp.exposure_time, exp.ae_luma);
		return 1;
	}

	if (strstr(cmd_json, "\"set-running-mode\"")) {
		char mode_str[8];
		if (rvd_json_get_str(cmd_json, "value", mode_str, sizeof(mode_str)) == 0) {
			int mode = (strcmp(mode_str, "night") == 0) ? 1 : 0;
			RSS_HAL_CALL(st->ops, isp_set_running_mode, st->hal_ctx, mode);
			snprintf(resp, resp_size, "{\"status\":\"ok\",\"mode\":\"%s\"}",
				 mode ? "night" : "day");
		} else {
			snprintf(resp, resp_size, "{\"status\":\"error\"}");
		}
		return 1;
	}

	return 0;
}

/* ── IVS commands ── */

static int handle_ivs_cmd(const char *cmd_json, rvd_state_t *st, char *resp, int resp_size)
{
	if (strstr(cmd_json, "\"ivs-status\"")) {
		bool motion = atomic_load(&st->ivs_motion);
		int64_t ts = atomic_load(&st->ivs_motion_ts);
		snprintf(resp, resp_size,
			 "{\"status\":\"ok\",\"active\":%s,\"motion\":%s,\"timestamp\":%" PRId64 "}",
			 st->ivs_active ? "true" : "false", motion ? "true" : "false", ts);
		return 1;
	}

	if (strstr(cmd_json, "\"ivs-set-sensitivity\"")) {
		int sens = -1;
		if (rvd_json_get_int(cmd_json, "value", &sens) == 0 && st->ivs_active && sens >= 0) {
			rss_ivs_move_param_t mp;
			memset(&mp, 0, sizeof(mp));
			if (RSS_HAL_CALL(st->ops, ivs_get_param, st->hal_ctx, st->ivs_chn, &mp) == 0) {
				for (int i = 0; i < mp.roi_count; i++)
					mp.sense[i] = sens;
				RSS_HAL_CALL(st->ops, ivs_set_param, st->hal_ctx, st->ivs_chn, &mp);
			}
			snprintf(resp, resp_size, "{\"status\":\"ok\",\"sensitivity\":%d}", sens);
		} else {
			snprintf(resp, resp_size,
				 "{\"status\":\"error\",\"reason\":\"invalid or ivs not active\"}");
		}
		return 1;
	}

	return 0;
}

/* ── Config and status commands ── */

static int handle_config_cmd(const char *cmd_json, rvd_state_t *st, char *resp, int resp_size)
{
	if (strstr(cmd_json, "\"config-get\"")) {
		char section[64], key[64];
		if (rvd_json_get_str(cmd_json, "section", section, sizeof(section)) == 0 &&
		    rvd_json_get_str(cmd_json, "key", key, sizeof(key)) == 0) {
			const char *v = rss_config_get_str(st->cfg, section, key, NULL);
			if (v)
				snprintf(resp, resp_size, "%s", v);
			else
				resp[0] = '\0';
		} else {
			resp[0] = '\0';
		}
		return 1;
	}

	if (strstr(cmd_json, "\"config-save\"")) {
		int ret = rss_config_save(st->cfg, st->config_path);
		snprintf(resp, resp_size, "{\"status\":\"%s\"}", ret == 0 ? "ok" : "error");
		if (ret == 0)
			RSS_INFO("running config saved to %s", st->config_path);
		return 1;
	}

	if (strstr(cmd_json, "\"config-show\"")) {
		int off = snprintf(resp, resp_size, "{\"status\":\"ok\",\"config\":{\"streams\":[");
		for (int i = 0; i < st->stream_count; i++) {
			rvd_stream_t *s = &st->streams[i];
			uint32_t avg_br = 0;
			RSS_HAL_CALL(st->ops, enc_get_avg_bitrate, st->hal_ctx, s->chn, &avg_br);
			off += snprintf(resp + off, resp_size - off,
					"%s{\"chn\":%d,\"w\":%u,\"h\":%u,\"codec\":%u,"
					"\"bitrate\":%u,\"avg_bitrate\":%u,\"gop\":%u,"
					"\"fps\":%u,\"min_qp\":%d,\"max_qp\":%d,"
					"\"rc_mode\":%u,\"profile\":%d}",
					i > 0 ? "," : "", s->chn, s->enc_cfg.width,
					s->enc_cfg.height, s->enc_cfg.codec, s->enc_cfg.bitrate,
					avg_br, s->enc_cfg.gop_length, s->enc_cfg.fps_num,
					s->enc_cfg.min_qp, s->enc_cfg.max_qp, s->enc_cfg.rc_mode,
					s->enc_cfg.profile);
		}
		snprintf(resp + off, resp_size - off, "],\"config_path\":\"%s\"}}",
			 st->config_path);
		return 1;
	}

	if (strstr(cmd_json, "\"privacy\"")) {
		char val_str[8];
		bool enable;
		if (rvd_json_get_str(cmd_json, "value", val_str, sizeof(val_str)) == 0)
			enable = (strcmp(val_str, "on") == 0 || strcmp(val_str, "1") == 0);
		else
			enable = !st->privacy_active;
		rvd_osd_set_privacy(st, enable);
		snprintf(resp, resp_size, "{\"status\":\"ok\",\"privacy\":\"%s\"}",
			 st->privacy_active ? "on" : "off");
		return 1;
	}

	if (strstr(cmd_json, "\"status\"")) {
		int off = snprintf(resp, resp_size, "{\"status\":\"ok\",\"streams\":[");
		for (int i = 0; i < st->stream_count; i++) {
			uint32_t avg_br = 0;
			RSS_HAL_CALL(st->ops, enc_get_avg_bitrate, st->hal_ctx, st->streams[i].chn,
				     &avg_br);
			off += snprintf(resp + off, resp_size - off,
					"%s{\"chn\":%d,\"w\":%u,\"h\":%u,\"codec\":%u,"
					"\"bitrate\":%u,\"avg_bitrate\":%u,\"gop\":%u,"
					"\"fps\":%u}",
					i > 0 ? "," : "", st->streams[i].chn,
					st->streams[i].enc_cfg.width, st->streams[i].enc_cfg.height,
					st->streams[i].enc_cfg.codec, st->streams[i].enc_cfg.bitrate,
					avg_br, st->streams[i].enc_cfg.gop_length,
					st->streams[i].enc_cfg.fps_num);
		}
		snprintf(resp + off, resp_size - off, "]}");
		return 1;
	}

	return 0;
}

/* ── Main dispatch ── */

int rvd_ctrl_handler(const char *cmd_json, char *resp_buf, int resp_buf_size, void *userdata)
{
	rvd_state_t *st = userdata;

	if (handle_encoder_cmd(cmd_json, st, resp_buf, resp_buf_size))
		CTRL_RETURN(resp_buf);

	if (handle_isp_cmd(cmd_json, st, resp_buf, resp_buf_size))
		CTRL_RETURN(resp_buf);

	if (handle_ivs_cmd(cmd_json, st, resp_buf, resp_buf_size))
		CTRL_RETURN(resp_buf);

	if (handle_config_cmd(cmd_json, st, resp_buf, resp_buf_size))
		CTRL_RETURN(resp_buf);

	snprintf(resp_buf, resp_buf_size, "{\"status\":\"error\",\"reason\":\"unknown command\"}");
	CTRL_RETURN(resp_buf);
}

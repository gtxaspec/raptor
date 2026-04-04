/*
 * rvd_ctrl.c -- Control socket command handler
 *
 * Handles raptorctl commands for encoder settings, ISP tuning,
 * config management, privacy mode, and IVS status.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <inttypes.h>
#include <stdatomic.h>

#include "rvd.h"

/* White balance mode name lookup */
static const struct {
	const char *name;
	rss_wb_mode_t mode;
} wb_mode_table[] = {
	{"auto",             RSS_WB_AUTO},
	{"manual",           RSS_WB_MANUAL},
	{"daylight",         RSS_WB_DAYLIGHT},
	{"cloudy",           RSS_WB_CLOUDY},
	{"incandescent",     RSS_WB_INCANDESCENT},
	{"flourescent",      RSS_WB_FLOURESCENT},
	{"twilight",         RSS_WB_TWILIGHT},
	{"shade",            RSS_WB_SHADE},
	{"warm_flourescent", RSS_WB_WARM_FLOURESCENT},
	{"custom",           RSS_WB_CUSTOM},
};
#define WB_MODE_COUNT (sizeof(wb_mode_table) / sizeof(wb_mode_table[0]))

static const char *wb_mode_str(rss_wb_mode_t m)
{
	for (unsigned i = 0; i < WB_MODE_COUNT; i++)
		if (wb_mode_table[i].mode == m)
			return wb_mode_table[i].name;
	return "unknown";
}

static rss_wb_mode_t wb_mode_from_str(const char *s)
{
	for (unsigned i = 0; i < WB_MODE_COUNT; i++)
		if (strcasecmp(wb_mode_table[i].name, s) == 0)
			return wb_mode_table[i].mode;
	return RSS_WB_AUTO;
}

/* ── Helpers ── */

/*
 * Control handler return convention: return value = response length.
 * The IPC layer uses it as the number of bytes to send back.
 */
#define CTRL_RETURN(buf) return (int)strlen(buf)

/* Format HAL return code into JSON error response */
static void fmt_hal_result(char *buf, int bufsz, int ret)
{
	if (ret == 0)
		snprintf(buf, bufsz, "{\"status\":\"ok\"}");
	else if (ret == RSS_ERR_NOTSUP)
		snprintf(buf, bufsz, "{\"status\":\"error\",\"reason\":\"not supported on this SoC\"}");
	else
		snprintf(buf, bufsz, "{\"status\":\"error\",\"reason\":\"failed (%d)\"}", ret);
}

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
		rss_json_get_int(cmd_json, "channel", &target);
		for (int i = 0; i < st->stream_count; i++) {
			if (target >= 0 && i != target)
				continue;
			RSS_HAL_CALL(st->ops, enc_request_idr, st->hal_ctx, st->streams[i].chn);
		}
		snprintf(resp, resp_size, "{\"status\":\"ok\"}");
		return 1;
	}

	if (strstr(cmd_json, "\"set-rc-mode\"")) {
		char mode_str[20] = "";
		rss_json_get_str(cmd_json, "mode", mode_str, sizeof(mode_str));
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 && chn >= 0 &&
		    chn < st->stream_count && mode_str[0]) {
			static const struct { const char *name; rss_rc_mode_t mode; } rc_map[] = {
				{"fixqp",          RSS_RC_FIXQP},
				{"cbr",            RSS_RC_CBR},
				{"vbr",            RSS_RC_VBR},
				{"smart",          RSS_RC_SMART},
				{"capped_vbr",     RSS_RC_CAPPED_VBR},
				{"capped_quality", RSS_RC_CAPPED_QUALITY},
			};
			rss_rc_mode_t mode = RSS_RC_CBR;
			for (unsigned i = 0; i < sizeof(rc_map) / sizeof(rc_map[0]); i++) {
				if (strcasecmp(rc_map[i].name, mode_str) == 0) {
					mode = rc_map[i].mode;
					break;
				}
			}
			uint32_t bitrate = st->streams[chn].enc_cfg.bitrate;
			int br;
			if (rss_json_get_int(cmd_json, "bitrate", &br) == 0 && br > 0)
				bitrate = (uint32_t)br;
			int ret = RSS_HAL_CALL(st->ops, enc_set_rc_mode, st->hal_ctx,
					       st->streams[chn].chn, mode, bitrate);
			if (ret == 0) {
				st->streams[chn].enc_cfg.rc_mode = mode;
				rss_config_set_str(st->cfg, stream_section(chn), "rc_mode", mode_str);
			}
			snprintf(resp, resp_size, "{\"status\":\"%s\",\"rc_mode\":\"%s\"}",
				 ret == 0 ? "ok" : "error", mode_str);
		} else {
			snprintf(resp, resp_size,
				 "{\"status\":\"error\",\"reason\":\"need channel and mode\"}");
		}
		return 1;
	}

	if (strstr(cmd_json, "\"set-bitrate\"")) {
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 &&
		    rss_json_get_int(cmd_json, "value", &val) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			int ret = RSS_HAL_CALL(st->ops, enc_set_bitrate, st->hal_ctx,
					       st->streams[chn].chn, val);
			if (ret == 0) {
				st->streams[chn].enc_cfg.bitrate = val;
				rss_config_set_int(st->cfg, stream_section(chn), "bitrate", val);
			}
			fmt_hal_result(resp, resp_size, ret);
		} else {
			snprintf(resp, resp_size,
				 "{\"status\":\"error\",\"reason\":\"need channel and value\"}");
		}
		return 1;
	}

	if (strstr(cmd_json, "\"set-gop\"")) {
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 &&
		    rss_json_get_int(cmd_json, "value", &val) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			int ret = RSS_HAL_CALL(st->ops, enc_set_gop, st->hal_ctx,
					       st->streams[chn].chn, val);
			if (ret == 0) {
				st->streams[chn].enc_cfg.gop_length = val;
				rss_config_set_int(st->cfg, stream_section(chn), "gop", val);
			}
			fmt_hal_result(resp, resp_size, ret);
		} else {
			snprintf(resp, resp_size,
				 "{\"status\":\"error\",\"reason\":\"need channel and value\"}");
		}
		return 1;
	}

	if (strstr(cmd_json, "\"set-fps\"")) {
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 &&
		    rss_json_get_int(cmd_json, "value", &val) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			int ret = RSS_HAL_CALL(st->ops, enc_set_fps, st->hal_ctx,
					       st->streams[chn].chn, val, 1);
			if (ret == 0) {
				st->streams[chn].enc_cfg.fps_num = val;
				rss_config_set_int(st->cfg, stream_section(chn), "fps", val);
			}
			fmt_hal_result(resp, resp_size, ret);
		} else {
			snprintf(resp, resp_size,
				 "{\"status\":\"error\",\"reason\":\"need channel and value\"}");
		}
		return 1;
	}

	if (strstr(cmd_json, "\"set-qp-bounds\"")) {
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 &&
		    rss_json_get_int(cmd_json, "min", &val) == 0 &&
		    rss_json_get_int(cmd_json, "max", &val2) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			int ret = RSS_HAL_CALL(st->ops, enc_set_qp_bounds, st->hal_ctx,
					       st->streams[chn].chn, val, val2);
			if (ret == 0) {
				st->streams[chn].enc_cfg.min_qp = val;
				st->streams[chn].enc_cfg.max_qp = val2;
				rss_config_set_int(st->cfg, stream_section(chn), "min_qp", val);
				rss_config_set_int(st->cfg, stream_section(chn), "max_qp", val2);
			}
			fmt_hal_result(resp, resp_size, ret);
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
	int sensor_idx = -1;
	rss_json_get_int(cmd_json, "sensor", &sensor_idx);

/* ISP_SET_N: supports --sensor N via _n variant */
#define ISP_SET_N(name, fn)                                                                        \
	if (strstr(cmd_json, "\"" name "\"")) {                                                    \
		if (rss_json_get_int(cmd_json, "value", &val) == 0) {                              \
			int ret;                                                                   \
			if (sensor_idx >= 0)                                                       \
				ret = RSS_HAL_CALL(st->ops, fn##_n, st->hal_ctx, sensor_idx, val); \
			else                                                                       \
				ret = RSS_HAL_CALL(st->ops, fn, st->hal_ctx, val);                 \
			fmt_hal_result(resp, resp_size, ret);                                      \
		} else {                                                                           \
			snprintf(resp, resp_size,                                                  \
				 "{\"status\":\"error\",\"reason\":\"need value\"}");              \
		}                                                                                  \
		return 1;                                                                          \
	}

/* ISP_SET: single-sensor only (no _n variant) */
#define ISP_SET(name, fn)                                                                          \
	if (strstr(cmd_json, "\"" name "\"")) {                                                    \
		if (rss_json_get_int(cmd_json, "value", &val) == 0) {                              \
			int ret = RSS_HAL_CALL(st->ops, fn, st->hal_ctx, val);                     \
			fmt_hal_result(resp, resp_size, ret);                                      \
		} else {                                                                           \
			snprintf(resp, resp_size,                                                  \
				 "{\"status\":\"error\",\"reason\":\"need value\"}");              \
		}                                                                                  \
		return 1;                                                                          \
	}

	ISP_SET_N("set-brightness", isp_set_brightness)
	ISP_SET_N("set-contrast", isp_set_contrast)
	ISP_SET_N("set-saturation", isp_set_saturation)
	ISP_SET_N("set-sharpness", isp_set_sharpness)
	ISP_SET_N("set-hue", isp_set_hue)
	ISP_SET_N("set-sinter", isp_set_sinter_strength)
	ISP_SET_N("set-temper", isp_set_temper_strength)
	ISP_SET("set-dpc", isp_set_dpc_strength)
	ISP_SET("set-drc", isp_set_drc_strength)
	ISP_SET("set-highlight-depress", isp_set_highlight_depress)
	ISP_SET_N("set-ae-comp", isp_set_ae_comp)
	ISP_SET_N("set-max-again", isp_set_max_again)
	ISP_SET_N("set-max-dgain", isp_set_max_dgain)
	ISP_SET_N("set-hflip", isp_set_hflip)
	ISP_SET_N("set-vflip", isp_set_vflip)
	ISP_SET("set-defog", isp_set_defog)
	ISP_SET_N("set-antiflicker", isp_set_antiflicker)

#undef ISP_SET_N
#undef ISP_SET

	if (strstr(cmd_json, "\"set-wb\"")) {
		rss_wb_config_t wb = {0};
		RSS_HAL_CALL(st->ops, isp_get_wb, st->hal_ctx, &wb);
		char mode_str[20] = "";
		rss_json_get_str(cmd_json, "mode", mode_str, sizeof(mode_str));
		if (mode_str[0])
			wb.mode = wb_mode_from_str(mode_str);
		int r_gain, b_gain;
		if (rss_json_get_int(cmd_json, "r_gain", &r_gain) == 0)
			wb.r_gain = (uint16_t)r_gain;
		if (rss_json_get_int(cmd_json, "b_gain", &b_gain) == 0)
			wb.b_gain = (uint16_t)b_gain;
		int ret = RSS_HAL_CALL(st->ops, isp_set_wb, st->hal_ctx, &wb);
		snprintf(resp, resp_size,
			 "{\"status\":\"%s\",\"mode\":\"%s\",\"r_gain\":%u,\"b_gain\":%u}",
			 ret == 0 ? "ok" : "error", wb_mode_str(wb.mode),
			 wb.r_gain, wb.b_gain);
		return 1;
	}

	if (strstr(cmd_json, "\"get-wb\"")) {
		rss_wb_config_t wb = {0};
		RSS_HAL_CALL(st->ops, isp_get_wb, st->hal_ctx, &wb);
		snprintf(resp, resp_size,
			 "{\"status\":\"ok\",\"mode\":\"%s\",\"r_gain\":%u,\"g_gain\":%u,\"b_gain\":%u}",
			 wb_mode_str(wb.mode), wb.r_gain, wb.g_gain, wb.b_gain);
		return 1;
	}

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
		rss_wb_config_t wb = {0};
		RSS_HAL_CALL(st->ops, isp_get_wb, st->hal_ctx, &wb);
		snprintf(resp, resp_size,
			 "{\"status\":\"ok\","
			 "\"brightness\":%u,\"contrast\":%u,\"saturation\":%u,"
			 "\"sharpness\":%u,\"hue\":%u,\"sinter\":%u,\"temper\":%u,"
			 "\"hflip\":%d,\"vflip\":%d,\"ae_comp\":%d,"
			 "\"max_again\":%u,\"max_dgain\":%u,"
			 "\"wb_mode\":\"%s\",\"wb_r\":%u,\"wb_g\":%u,\"wb_b\":%u}",
			 bri, con, sat, shp, hue, sin, tem, hf, vf, ae, again, dgain,
			 wb.mode == RSS_WB_MANUAL ? "manual" : "auto",
			 wb.r_gain, wb.g_gain, wb.b_gain);
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
		if (rss_json_get_str(cmd_json, "value", mode_str, sizeof(mode_str)) == 0) {
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
			 "{\"status\":\"ok\",\"active\":%s,\"motion\":%s,\"timestamp\":%" PRId64
			 "}",
			 st->ivs_active ? "true" : "false", motion ? "true" : "false", ts);
		return 1;
	}

	if (strstr(cmd_json, "\"ivs-set-sensitivity\"")) {
		int sens = -1;
		if (rss_json_get_int(cmd_json, "value", &sens) == 0 && st->ivs_active &&
		    sens >= 0) {
			rss_ivs_move_param_t mp;
			memset(&mp, 0, sizeof(mp));
			if (RSS_HAL_CALL(st->ops, ivs_get_param, st->hal_ctx, st->ivs_chn, &mp) ==
			    0) {
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
	int rc = rss_ctrl_handle_common(cmd_json, resp, resp_size, st->cfg, st->config_path);
	if (rc >= 0)
		return 1;

	if (strstr(cmd_json, "\"config-show\"")) {
		int off = snprintf(resp, resp_size, "{\"status\":\"ok\",\"config\":{\"streams\":[");
		for (int i = 0; i < st->stream_count && off < resp_size; i++) {
			rvd_stream_t *s = &st->streams[i];
			uint32_t avg_br = 0;
			RSS_HAL_CALL(st->ops, enc_get_avg_bitrate, st->hal_ctx, s->chn, &avg_br);
			int n = snprintf(resp + off, resp_size - off,
					 "%s{\"chn\":%d,\"w\":%u,\"h\":%u,\"codec\":%u,"
					 "\"bitrate\":%u,\"avg_bitrate\":%u,\"gop\":%u,"
					 "\"fps\":%u,\"min_qp\":%d,\"max_qp\":%d,"
					 "\"rc_mode\":%u,\"profile\":%d}",
					 i > 0 ? "," : "", s->chn, s->enc_cfg.width,
					 s->enc_cfg.height, s->enc_cfg.codec, s->enc_cfg.bitrate,
					 avg_br, s->enc_cfg.gop_length, s->enc_cfg.fps_num,
					 s->enc_cfg.min_qp, s->enc_cfg.max_qp, s->enc_cfg.rc_mode,
					 s->enc_cfg.profile);
			if (n > 0 && off + n < resp_size)
				off += n;
		}
		if (off < resp_size)
			snprintf(resp + off, resp_size - off, "],\"config_path\":\"%s\"}}",
				 st->config_path);
		return 1;
	}

	if (strstr(cmd_json, "\"privacy\"")) {
		char val_str[8];
		bool enable;
		if (rss_json_get_str(cmd_json, "value", val_str, sizeof(val_str)) == 0)
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
		for (int i = 0; i < st->stream_count && off < resp_size; i++) {
			uint32_t avg_br = 0;
			RSS_HAL_CALL(st->ops, enc_get_avg_bitrate, st->hal_ctx, st->streams[i].chn,
				     &avg_br);
			int n = snprintf(
				resp + off, resp_size - off,
				"%s{\"chn\":%d,\"w\":%u,\"h\":%u,\"codec\":%u,"
				"\"bitrate\":%u,\"avg_bitrate\":%u,\"gop\":%u,"
				"\"fps\":%u}",
				i > 0 ? "," : "", st->streams[i].chn, st->streams[i].enc_cfg.width,
				st->streams[i].enc_cfg.height, st->streams[i].enc_cfg.codec,
				st->streams[i].enc_cfg.bitrate, avg_br,
				st->streams[i].enc_cfg.gop_length, st->streams[i].enc_cfg.fps_num);
			if (n > 0 && off + n < resp_size)
				off += n;
		}
		if (off < resp_size)
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

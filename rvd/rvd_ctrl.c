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
	{"auto", RSS_WB_AUTO},
	{"manual", RSS_WB_MANUAL},
	{"daylight", RSS_WB_DAYLIGHT},
	{"cloudy", RSS_WB_CLOUDY},
	{"incandescent", RSS_WB_INCANDESCENT},
	{"flourescent", RSS_WB_FLOURESCENT},
	{"twilight", RSS_WB_TWILIGHT},
	{"shade", RSS_WB_SHADE},
	{"warm_flourescent", RSS_WB_WARM_FLOURESCENT},
	{"custom", RSS_WB_CUSTOM},
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
static int fmt_hal_result(char *buf, int bufsz, int ret)
{
	if (ret == 0)
		return rss_ctrl_resp_ok(buf, bufsz);
	else if (ret == RSS_ERR_NOTSUP)
		return rss_ctrl_resp_error(buf, bufsz, "not supported on this SoC");
	else
		return rss_ctrl_resp(buf, bufsz,
				     "{\"status\":\"error\",\"reason\":\"failed (%d)\"}", ret);
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
		rss_ctrl_resp_ok(resp, resp_size);
		return 1;
	}

	if (strstr(cmd_json, "\"set-rc-mode\"")) {
		char mode_str[20] = "";
		rss_json_get_str(cmd_json, "mode", mode_str, sizeof(mode_str));
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 && chn >= 0 &&
		    chn < st->stream_count && mode_str[0]) {
			static const struct {
				const char *name;
				rss_rc_mode_t mode;
			} rc_map[] = {
				{"fixqp", RSS_RC_FIXQP},
				{"cbr", RSS_RC_CBR},
				{"vbr", RSS_RC_VBR},
				{"smart", RSS_RC_SMART},
				{"capped_vbr", RSS_RC_CAPPED_VBR},
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
				rss_config_set_str(st->cfg, st->streams[chn].cfg_sect, "rc_mode",
						   mode_str);
			}
			rss_ctrl_resp(resp, resp_size, "{\"status\":\"%s\",\"rc_mode\":\"%s\"}",
				      ret == 0 ? "ok" : "error", mode_str);
		} else {
			rss_ctrl_resp_error(resp, resp_size, "need channel and mode");
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
				rss_config_set_int(st->cfg, st->streams[chn].cfg_sect, "bitrate",
						   val);
			}
			fmt_hal_result(resp, resp_size, ret);
		} else {
			rss_ctrl_resp_error(resp, resp_size, "need channel and value");
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
				rss_config_set_int(st->cfg, st->streams[chn].cfg_sect, "gop", val);
			}
			fmt_hal_result(resp, resp_size, ret);
		} else {
			rss_ctrl_resp_error(resp, resp_size, "need channel and value");
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
				rss_config_set_int(st->cfg, st->streams[chn].cfg_sect, "fps", val);
			}
			fmt_hal_result(resp, resp_size, ret);
		} else {
			rss_ctrl_resp_error(resp, resp_size, "need channel and value");
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
				rss_config_set_int(st->cfg, st->streams[chn].cfg_sect, "min_qp",
						   val);
				rss_config_set_int(st->cfg, st->streams[chn].cfg_sect, "max_qp",
						   val2);
			}
			fmt_hal_result(resp, resp_size, ret);
		} else {
			rss_ctrl_resp_error(resp, resp_size, "need channel, min, max");
		}
		return 1;
	}

	if (strstr(cmd_json, "\"get-bitrate\"")) {
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			uint32_t avg = 0;
			RSS_HAL_CALL(st->ops, enc_get_avg_bitrate, st->hal_ctx,
				     st->streams[chn].chn, &avg);
			rss_ctrl_resp(resp, resp_size,
				      "{\"status\":\"ok\",\"bitrate\":%u,\"avg_bitrate\":%u}",
				      st->streams[chn].enc_cfg.bitrate, avg);
		} else {
			rss_ctrl_resp_error(resp, resp_size, "need channel");
		}
		return 1;
	}

	if (strstr(cmd_json, "\"get-fps\"")) {
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			uint32_t num = 0, den = 0;
			RSS_HAL_CALL(st->ops, enc_get_fps, st->hal_ctx, st->streams[chn].chn, &num,
				     &den);
			rss_ctrl_resp(resp, resp_size,
				      "{\"status\":\"ok\",\"fps_num\":%u,\"fps_den\":%u}", num,
				      den);
		} else {
			rss_ctrl_resp_error(resp, resp_size, "need channel");
		}
		return 1;
	}

	if (strstr(cmd_json, "\"get-gop\"")) {
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			uint32_t gop = 0;
			RSS_HAL_CALL(st->ops, enc_get_gop_attr, st->hal_ctx, st->streams[chn].chn,
				     &gop);
			rss_ctrl_resp(resp, resp_size, "{\"status\":\"ok\",\"gop\":%u}", gop);
		} else {
			rss_ctrl_resp_error(resp, resp_size, "need channel");
		}
		return 1;
	}

	if (strstr(cmd_json, "\"get-qp-bounds\"")) {
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			rss_ctrl_resp(
				resp, resp_size, "{\"status\":\"ok\",\"min_qp\":%d,\"max_qp\":%d}",
				st->streams[chn].enc_cfg.min_qp, st->streams[chn].enc_cfg.max_qp);
		} else {
			rss_ctrl_resp_error(resp, resp_size, "need channel");
		}
		return 1;
	}

	if (strstr(cmd_json, "\"get-rc-mode\"")) {
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			static const char *rc_names[] = {"fixqp", "cbr",	"vbr",
							 "smart", "capped_vbr", "capped_quality"};
			int mode = st->streams[chn].enc_cfg.rc_mode;
			const char *name =
				(mode >= 0 && mode < (int)(sizeof(rc_names) / sizeof(rc_names[0])))
					? rc_names[mode]
					: "unknown";
			rss_ctrl_resp(resp, resp_size,
				      "{\"status\":\"ok\",\"rc_mode\":\"%s\",\"rc_mode_id\":%d}",
				      name, mode);
		} else {
			rss_ctrl_resp_error(resp, resp_size, "need channel");
		}
		return 1;
	}

	return 0; /* not an encoder command */
}

/* ── Advanced encoder commands ── */

static int handle_encoder_advanced_cmd(const char *cmd_json, rvd_state_t *st, char *resp,
				       int resp_size)
{
	int chn, val, val2;
	const rss_hal_caps_t *caps = st->ops->get_caps ? st->ops->get_caps(st->hal_ctx) : NULL;

/* Simple set: channel + value → HAL call with int arg */
#define ENC_SET_INT(name, fn)                                                                      \
	if (strstr(cmd_json, "\"" name "\"")) {                                                    \
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 &&                            \
		    rss_json_get_int(cmd_json, "value", &val) == 0 && chn >= 0 &&                  \
		    chn < st->stream_count) {                                                      \
			int ret =                                                                  \
				RSS_HAL_CALL(st->ops, fn, st->hal_ctx, st->streams[chn].chn, val); \
			fmt_hal_result(resp, resp_size, ret);                                      \
		} else {                                                                           \
			rss_ctrl_resp_error(resp, resp_size, "need channel and value");            \
		}                                                                                  \
		return 1;                                                                          \
	}

/* Simple set: channel + bool value */
#define ENC_SET_BOOL(name, fn)                                                                     \
	if (strstr(cmd_json, "\"" name "\"")) {                                                    \
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 &&                            \
		    rss_json_get_int(cmd_json, "value", &val) == 0 && chn >= 0 &&                  \
		    chn < st->stream_count) {                                                      \
			int ret = RSS_HAL_CALL(st->ops, fn, st->hal_ctx, st->streams[chn].chn,     \
					       (bool)val);                                         \
			fmt_hal_result(resp, resp_size, ret);                                      \
		} else {                                                                           \
			rss_ctrl_resp_error(resp, resp_size, "need channel and value");            \
		}                                                                                  \
		return 1;                                                                          \
	}

/* Simple get: channel → HAL call returning uint32_t */
#define ENC_GET_U32(name, fn, field)                                                               \
	if (strstr(cmd_json, "\"" name "\"")) {                                                    \
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 && chn >= 0 &&                \
		    chn < st->stream_count) {                                                      \
			uint32_t out = 0;                                                          \
			int ret = RSS_HAL_CALL(st->ops, fn, st->hal_ctx, st->streams[chn].chn,     \
					       &out);                                              \
			if (ret == 0)                                                              \
				rss_ctrl_resp(resp, resp_size,                                     \
					      "{\"status\":\"ok\",\"" field "\":%u}", out);        \
			else                                                                       \
				fmt_hal_result(resp, resp_size, ret);                              \
		} else {                                                                           \
			rss_ctrl_resp_error(resp, resp_size, "need channel");                      \
		}                                                                                  \
		return 1;                                                                          \
	}

/* Simple get: channel → HAL call returning int */
#define ENC_GET_INT(name, fn, field)                                                               \
	if (strstr(cmd_json, "\"" name "\"")) {                                                    \
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 && chn >= 0 &&                \
		    chn < st->stream_count) {                                                      \
			int out = 0;                                                               \
			int ret = RSS_HAL_CALL(st->ops, fn, st->hal_ctx, st->streams[chn].chn,     \
					       &out);                                              \
			if (ret == 0)                                                              \
				rss_ctrl_resp(resp, resp_size,                                     \
					      "{\"status\":\"ok\",\"" field "\":%d}", out);        \
			else                                                                       \
				fmt_hal_result(resp, resp_size, ret);                              \
		} else {                                                                           \
			rss_ctrl_resp_error(resp, resp_size, "need channel");                      \
		}                                                                                  \
		return 1;                                                                          \
	}

/* Simple get: channel → HAL call returning bool */
#define ENC_GET_BOOL(name, fn, field)                                                              \
	if (strstr(cmd_json, "\"" name "\"")) {                                                    \
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 && chn >= 0 &&                \
		    chn < st->stream_count) {                                                      \
			bool out = false;                                                          \
			int ret = RSS_HAL_CALL(st->ops, fn, st->hal_ctx, st->streams[chn].chn,     \
					       &out);                                              \
			if (ret == 0)                                                              \
				rss_ctrl_resp(resp, resp_size,                                     \
					      "{\"status\":\"ok\",\"" field "\":%s}",              \
					      out ? "true" : "false");                             \
			else                                                                       \
				fmt_hal_result(resp, resp_size, ret);                              \
		} else {                                                                           \
			rss_ctrl_resp_error(resp, resp_size, "need channel");                      \
		}                                                                                  \
		return 1;                                                                          \
	}

	/* ── Simple int/bool set commands ── */
	ENC_SET_INT("set-qp", enc_set_qp)
	ENC_SET_INT("set-qp-ip-delta", enc_set_qp_ip_delta)
	ENC_SET_INT("set-qp-pb-delta", enc_set_qp_pb_delta)
	ENC_SET_INT("set-max-psnr", enc_set_max_psnr)
	ENC_SET_INT("set-gop-mode", enc_set_gop_mode)
	ENC_SET_INT("set-rc-options", enc_set_rc_options)
	ENC_SET_INT("set-max-same-scene", enc_set_max_same_scene_cnt)
	ENC_SET_INT("set-qpg-mode", enc_set_qpg_mode)
	ENC_SET_INT("set-entropy-mode", enc_set_chn_entropy_mode)
	ENC_SET_INT("set-stream-buf-size", enc_set_stream_buf_size)
	ENC_SET_INT("set-jpeg-qp", enc_set_jpeg_qp)
	ENC_SET_BOOL("set-color2grey", enc_set_color2grey)
	ENC_SET_BOOL("set-mbrc", enc_set_mbrc)
	ENC_SET_BOOL("set-resize-mode", enc_set_resize_mode)

	/* ── Simple get commands ── */
	/* gop_mode: use dedicated handler for correct enum type */
	if (strstr(cmd_json, "\"get-gop-mode\"")) {
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			rss_gop_mode_t mode = RSS_GOP_DEFAULT;
			int ret = RSS_HAL_CALL(st->ops, enc_get_gop_mode, st->hal_ctx,
					       st->streams[chn].chn, &mode);
			if (ret == 0)
				rss_ctrl_resp(resp, resp_size,
					      "{\"status\":\"ok\",\"gop_mode\":%d}", (int)mode);
			else
				fmt_hal_result(resp, resp_size, ret);
		} else {
			rss_ctrl_resp_error(resp, resp_size, "need channel");
		}
		return 1;
	}
	ENC_GET_U32("get-rc-options", enc_get_rc_options, "rc_options")
	ENC_GET_U32("get-max-same-scene", enc_get_max_same_scene_cnt, "max_same_scene")
	ENC_GET_U32("get-stream-buf-size", enc_get_stream_buf_size, "buf_size")
	ENC_GET_INT("get-qpg-mode", enc_get_qpg_mode, "qpg_mode")
	ENC_GET_INT("get-jpeg-qp", enc_get_jpeg_qp, "jpeg_qp")
	ENC_GET_BOOL("get-color2grey", enc_get_color2grey, "color2grey")
	ENC_GET_BOOL("get-mbrc", enc_get_mbrc, "mbrc")

#undef ENC_SET_INT
#undef ENC_SET_BOOL
#undef ENC_GET_U32
#undef ENC_GET_INT
#undef ENC_GET_BOOL

	/* ── QP bounds per frame (I/P separate) ── */
	if (strstr(cmd_json, "\"set-qp-bounds-per-frame\"")) {
		int min_i, max_i, min_p, max_p;
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 &&
		    rss_json_get_int(cmd_json, "min_i", &min_i) == 0 &&
		    rss_json_get_int(cmd_json, "max_i", &max_i) == 0 &&
		    rss_json_get_int(cmd_json, "min_p", &min_p) == 0 &&
		    rss_json_get_int(cmd_json, "max_p", &max_p) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			int ret = RSS_HAL_CALL(st->ops, enc_set_qp_bounds_per_frame, st->hal_ctx,
					       st->streams[chn].chn, min_i, max_i, min_p, max_p);
			fmt_hal_result(resp, resp_size, ret);
		} else {
			rss_ctrl_resp_error(resp, resp_size,
					    "need channel, min_i, max_i, min_p, max_p");
		}
		return 1;
	}

	/* ── Max picture size ── */
	if (strstr(cmd_json, "\"set-max-pic-size\"")) {
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 &&
		    rss_json_get_int(cmd_json, "i_kbits", &val) == 0 &&
		    rss_json_get_int(cmd_json, "p_kbits", &val2) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			int ret = RSS_HAL_CALL(st->ops, enc_set_max_pic_size, st->hal_ctx,
					       st->streams[chn].chn, (uint32_t)val, (uint32_t)val2);
			fmt_hal_result(resp, resp_size, ret);
		} else {
			rss_ctrl_resp_error(resp, resp_size, "need channel, i_kbits, p_kbits");
		}
		return 1;
	}

	/* ── H.264 transform ── */
	if (strstr(cmd_json, "\"set-h264-trans\"")) {
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 &&
		    rss_json_get_int(cmd_json, "value", &val) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			rss_enc_h264_trans_t cfg = {.chroma_qp_index_offset = val};
			int ret = RSS_HAL_CALL(st->ops, enc_set_h264_trans, st->hal_ctx,
					       st->streams[chn].chn, &cfg);
			fmt_hal_result(resp, resp_size, ret);
		} else {
			rss_ctrl_resp_error(resp, resp_size, "need channel and value");
		}
		return 1;
	}

	if (strstr(cmd_json, "\"get-h264-trans\"")) {
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			rss_enc_h264_trans_t cfg = {0};
			int ret = RSS_HAL_CALL(st->ops, enc_get_h264_trans, st->hal_ctx,
					       st->streams[chn].chn, &cfg);
			if (ret == 0)
				rss_ctrl_resp(resp, resp_size,
					      "{\"status\":\"ok\",\"chroma_qp_offset\":%d}",
					      cfg.chroma_qp_index_offset);
			else
				fmt_hal_result(resp, resp_size, ret);
		} else {
			rss_ctrl_resp_error(resp, resp_size, "need channel");
		}
		return 1;
	}

	/* ── H.265 transform ── */
	if (strstr(cmd_json, "\"set-h265-trans\"")) {
		int cr_off, cb_off;
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 &&
		    rss_json_get_int(cmd_json, "cr_offset", &cr_off) == 0 &&
		    rss_json_get_int(cmd_json, "cb_offset", &cb_off) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			rss_enc_h265_trans_t cfg = {.chroma_cr_qp_offset = cr_off,
						    .chroma_cb_qp_offset = cb_off};
			int ret = RSS_HAL_CALL(st->ops, enc_set_h265_trans, st->hal_ctx,
					       st->streams[chn].chn, &cfg);
			fmt_hal_result(resp, resp_size, ret);
		} else {
			rss_ctrl_resp_error(resp, resp_size, "need channel, cr_offset, cb_offset");
		}
		return 1;
	}

	if (strstr(cmd_json, "\"get-h265-trans\"")) {
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			rss_enc_h265_trans_t cfg = {0};
			int ret = RSS_HAL_CALL(st->ops, enc_get_h265_trans, st->hal_ctx,
					       st->streams[chn].chn, &cfg);
			if (ret == 0)
				rss_ctrl_resp(resp, resp_size,
					      "{\"status\":\"ok\",\"cr_offset\":%d,"
					      "\"cb_offset\":%d}",
					      cfg.chroma_cr_qp_offset, cfg.chroma_cb_qp_offset);
			else
				fmt_hal_result(resp, resp_size, ret);
		} else {
			rss_ctrl_resp_error(resp, resp_size, "need channel");
		}
		return 1;
	}

	/* ── ROI ── */
	if (strstr(cmd_json, "\"set-roi\"")) {
		int idx, enable, x, y, w, h, qp;
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 &&
		    rss_json_get_int(cmd_json, "index", &idx) == 0 &&
		    rss_json_get_int(cmd_json, "enable", &enable) == 0 &&
		    rss_json_get_int(cmd_json, "x", &x) == 0 &&
		    rss_json_get_int(cmd_json, "y", &y) == 0 &&
		    rss_json_get_int(cmd_json, "w", &w) == 0 &&
		    rss_json_get_int(cmd_json, "h", &h) == 0 &&
		    rss_json_get_int(cmd_json, "qp", &qp) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			rss_enc_roi_t roi = {.index = (uint32_t)idx,
					     .enable = (bool)enable,
					     .qp = qp,
					     .x = x,
					     .y = y,
					     .w = w,
					     .h = h};
			int ret = RSS_HAL_CALL(st->ops, enc_set_roi, st->hal_ctx,
					       st->streams[chn].chn, &roi);
			fmt_hal_result(resp, resp_size, ret);
		} else {
			rss_ctrl_resp_error(resp, resp_size,
					    "need channel, index, enable, x, y, w, h, qp");
		}
		return 1;
	}

	if (strstr(cmd_json, "\"get-roi\"")) {
		int idx;
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 &&
		    rss_json_get_int(cmd_json, "index", &idx) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			rss_enc_roi_t roi = {0};
			int ret = RSS_HAL_CALL(st->ops, enc_get_roi, st->hal_ctx,
					       st->streams[chn].chn, (uint32_t)idx, &roi);
			if (ret == 0)
				rss_ctrl_resp(resp, resp_size,
					      "{\"status\":\"ok\",\"index\":%u,\"enable\":%s,"
					      "\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"qp\":%d}",
					      roi.index, roi.enable ? "true" : "false", roi.x,
					      roi.y, roi.w, roi.h, roi.qp);
			else
				fmt_hal_result(resp, resp_size, ret);
		} else {
			rss_ctrl_resp_error(resp, resp_size, "need channel and index");
		}
		return 1;
	}

	/* ── Super frame ── */
	if (strstr(cmd_json, "\"set-super-frame\"")) {
		int mode, i_thr, p_thr;
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 &&
		    rss_json_get_int(cmd_json, "mode", &mode) == 0 &&
		    rss_json_get_int(cmd_json, "i_thr", &i_thr) == 0 &&
		    rss_json_get_int(cmd_json, "p_thr", &p_thr) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			rss_super_frame_cfg_t cfg = {.mode = (rss_super_frame_mode_t)mode,
						     .i_bits_thr = (uint32_t)i_thr,
						     .p_bits_thr = (uint32_t)p_thr,
						     .priority = RSS_RC_PRIO_BITRATE};
			int ret = RSS_HAL_CALL(st->ops, enc_set_super_frame, st->hal_ctx,
					       st->streams[chn].chn, &cfg);
			fmt_hal_result(resp, resp_size, ret);
		} else {
			rss_ctrl_resp_error(resp, resp_size, "need channel, mode, i_thr, p_thr");
		}
		return 1;
	}

	if (strstr(cmd_json, "\"get-super-frame\"")) {
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			rss_super_frame_cfg_t cfg = {0};
			int ret = RSS_HAL_CALL(st->ops, enc_get_super_frame, st->hal_ctx,
					       st->streams[chn].chn, &cfg);
			if (ret == 0)
				rss_ctrl_resp(resp, resp_size,
					      "{\"status\":\"ok\",\"mode\":%d,"
					      "\"i_thr\":%u,\"p_thr\":%u,\"priority\":%d}",
					      cfg.mode, cfg.i_bits_thr, cfg.p_bits_thr,
					      cfg.priority);
			else
				fmt_hal_result(resp, resp_size, ret);
		} else {
			rss_ctrl_resp_error(resp, resp_size, "need channel");
		}
		return 1;
	}

	/* ── P-skip ── */
	if (strstr(cmd_json, "\"set-pskip\"")) {
		int enable, max_frames;
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 &&
		    rss_json_get_int(cmd_json, "enable", &enable) == 0 &&
		    rss_json_get_int(cmd_json, "max_frames", &max_frames) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			rss_pskip_cfg_t cfg = {.enable = (bool)enable, .max_frames = max_frames};
			int ret = RSS_HAL_CALL(st->ops, enc_set_pskip, st->hal_ctx,
					       st->streams[chn].chn, &cfg);
			fmt_hal_result(resp, resp_size, ret);
		} else {
			rss_ctrl_resp_error(resp, resp_size, "need channel, enable, max_frames");
		}
		return 1;
	}

	if (strstr(cmd_json, "\"get-pskip\"")) {
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			rss_pskip_cfg_t cfg = {0};
			int ret = RSS_HAL_CALL(st->ops, enc_get_pskip, st->hal_ctx,
					       st->streams[chn].chn, &cfg);
			if (ret == 0)
				rss_ctrl_resp(resp, resp_size,
					      "{\"status\":\"ok\",\"enable\":%s,"
					      "\"max_frames\":%d}",
					      cfg.enable ? "true" : "false", cfg.max_frames);
			else
				fmt_hal_result(resp, resp_size, ret);
		} else {
			rss_ctrl_resp_error(resp, resp_size, "need channel");
		}
		return 1;
	}

	if (strstr(cmd_json, "\"request-pskip\"")) {
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			int ret = RSS_HAL_CALL(st->ops, enc_request_pskip, st->hal_ctx,
					       st->streams[chn].chn);
			fmt_hal_result(resp, resp_size, ret);
		} else {
			rss_ctrl_resp_error(resp, resp_size, "need channel");
		}
		return 1;
	}

	/* ── SRD (static scene refresh) ── */
	if (strstr(cmd_json, "\"set-srd\"")) {
		int enable, level;
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 &&
		    rss_json_get_int(cmd_json, "enable", &enable) == 0 &&
		    rss_json_get_int(cmd_json, "level", &level) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			rss_srd_cfg_t cfg = {.enable = (bool)enable, .level = (uint8_t)level};
			int ret = RSS_HAL_CALL(st->ops, enc_set_srd, st->hal_ctx,
					       st->streams[chn].chn, &cfg);
			fmt_hal_result(resp, resp_size, ret);
		} else {
			rss_ctrl_resp_error(resp, resp_size, "need channel, enable, level");
		}
		return 1;
	}

	if (strstr(cmd_json, "\"get-srd\"")) {
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			rss_srd_cfg_t cfg = {0};
			int ret = RSS_HAL_CALL(st->ops, enc_get_srd, st->hal_ctx,
					       st->streams[chn].chn, &cfg);
			if (ret == 0)
				rss_ctrl_resp(resp, resp_size,
					      "{\"status\":\"ok\",\"enable\":%s,\"level\":%u}",
					      cfg.enable ? "true" : "false", cfg.level);
			else
				fmt_hal_result(resp, resp_size, ret);
		} else {
			rss_ctrl_resp_error(resp, resp_size, "need channel");
		}
		return 1;
	}

	/* ── Encoder denoise ── */
	if (strstr(cmd_json, "\"set-enc-denoise\"")) {
		int enable, dn_type, i_qp, p_qp;
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 &&
		    rss_json_get_int(cmd_json, "enable", &enable) == 0 &&
		    rss_json_get_int(cmd_json, "type", &dn_type) == 0 &&
		    rss_json_get_int(cmd_json, "i_qp", &i_qp) == 0 &&
		    rss_json_get_int(cmd_json, "p_qp", &p_qp) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			rss_enc_denoise_cfg_t cfg = {.enable = (bool)enable,
						     .dn_type = dn_type,
						     .dn_i_qp = i_qp,
						     .dn_p_qp = p_qp};
			int ret = RSS_HAL_CALL(st->ops, enc_set_denoise, st->hal_ctx,
					       st->streams[chn].chn, &cfg);
			fmt_hal_result(resp, resp_size, ret);
		} else {
			rss_ctrl_resp_error(resp, resp_size,
					    "need channel, enable, type, i_qp, p_qp");
		}
		return 1;
	}

	if (strstr(cmd_json, "\"get-enc-denoise\"")) {
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			rss_enc_denoise_cfg_t cfg = {0};
			int ret = RSS_HAL_CALL(st->ops, enc_get_denoise, st->hal_ctx,
					       st->streams[chn].chn, &cfg);
			if (ret == 0)
				rss_ctrl_resp(resp, resp_size,
					      "{\"status\":\"ok\",\"enable\":%s,"
					      "\"type\":%d,\"i_qp\":%d,\"p_qp\":%d}",
					      cfg.enable ? "true" : "false", cfg.dn_type,
					      cfg.dn_i_qp, cfg.dn_p_qp);
			else
				fmt_hal_result(resp, resp_size, ret);
		} else {
			rss_ctrl_resp_error(resp, resp_size, "need channel");
		}
		return 1;
	}

	/* ── GDR (gradual decoder refresh) ── */
	if (strstr(cmd_json, "\"set-gdr\"")) {
		int enable, cycle;
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 &&
		    rss_json_get_int(cmd_json, "enable", &enable) == 0 &&
		    rss_json_get_int(cmd_json, "cycle", &cycle) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			rss_enc_gdr_cfg_t cfg = {.enable = (bool)enable, .gdr_cycle = cycle};
			int ret = RSS_HAL_CALL(st->ops, enc_set_gdr, st->hal_ctx,
					       st->streams[chn].chn, &cfg);
			fmt_hal_result(resp, resp_size, ret);
		} else {
			rss_ctrl_resp_error(resp, resp_size, "need channel, enable, cycle");
		}
		return 1;
	}

	if (strstr(cmd_json, "\"get-gdr\"")) {
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			rss_enc_gdr_cfg_t cfg = {0};
			int ret = RSS_HAL_CALL(st->ops, enc_get_gdr, st->hal_ctx,
					       st->streams[chn].chn, &cfg);
			if (ret == 0)
				rss_ctrl_resp(resp, resp_size,
					      "{\"status\":\"ok\",\"enable\":%s,\"cycle\":%d}",
					      cfg.enable ? "true" : "false", cfg.gdr_cycle);
			else
				fmt_hal_result(resp, resp_size, ret);
		} else {
			rss_ctrl_resp_error(resp, resp_size, "need channel");
		}
		return 1;
	}

	if (strstr(cmd_json, "\"request-gdr\"")) {
		int frames;
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 &&
		    rss_json_get_int(cmd_json, "value", &frames) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			int ret = RSS_HAL_CALL(st->ops, enc_request_gdr, st->hal_ctx,
					       st->streams[chn].chn, frames);
			fmt_hal_result(resp, resp_size, ret);
		} else {
			rss_ctrl_resp_error(resp, resp_size, "need channel and value");
		}
		return 1;
	}

	/* ── Encoder crop ── */
	if (strstr(cmd_json, "\"set-enc-crop\"")) {
		int enable, x, y, w, h;
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 &&
		    rss_json_get_int(cmd_json, "enable", &enable) == 0 &&
		    rss_json_get_int(cmd_json, "x", &x) == 0 &&
		    rss_json_get_int(cmd_json, "y", &y) == 0 &&
		    rss_json_get_int(cmd_json, "w", &w) == 0 &&
		    rss_json_get_int(cmd_json, "h", &h) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			rss_enc_crop_cfg_t cfg = {.enable = (bool)enable,
						  .x = (uint32_t)x,
						  .y = (uint32_t)y,
						  .w = (uint32_t)w,
						  .h = (uint32_t)h};
			int ret = RSS_HAL_CALL(st->ops, enc_set_crop, st->hal_ctx,
					       st->streams[chn].chn, &cfg);
			fmt_hal_result(resp, resp_size, ret);
		} else {
			rss_ctrl_resp_error(resp, resp_size, "need channel, enable, x, y, w, h");
		}
		return 1;
	}

	if (strstr(cmd_json, "\"get-enc-crop\"")) {
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			rss_enc_crop_cfg_t cfg = {0};
			int ret = RSS_HAL_CALL(st->ops, enc_get_crop, st->hal_ctx,
					       st->streams[chn].chn, &cfg);
			if (ret == 0)
				rss_ctrl_resp(resp, resp_size,
					      "{\"status\":\"ok\",\"enable\":%s,"
					      "\"x\":%u,\"y\":%u,\"w\":%u,\"h\":%u}",
					      cfg.enable ? "true" : "false", cfg.x, cfg.y, cfg.w,
					      cfg.h);
			else
				fmt_hal_result(resp, resp_size, ret);
		} else {
			rss_ctrl_resp_error(resp, resp_size, "need channel");
		}
		return 1;
	}

	/* ── Encoder capabilities query ── */
	if (strstr(cmd_json, "\"get-enc-caps\"")) {
		if (!caps) {
			rss_ctrl_resp_error(resp, resp_size, "caps not available");
			return 1;
		}
		rss_ctrl_resp(
			resp, resp_size,
			"{\"status\":\"ok\""
			",\"smartp_gop\":%s,\"rc_options\":%s"
			",\"pskip\":%s,\"srd\":%s,\"max_pic_size\":%s"
			",\"super_frame\":%s,\"color2grey\":%s"
			",\"roi\":%s,\"map_roi\":%s"
			",\"qp_bounds_per_frame\":%s,\"qpg_mode\":%s"
			",\"mbrc\":%s,\"enc_denoise\":%s,\"gdr\":%s"
			",\"h264_trans\":%s,\"h265_trans\":%s"
			",\"enc_crop\":%s,\"resize_mode\":%s"
			",\"jpeg_ql\":%s,\"jpeg_qp\":%s}",
			caps->has_smartp_gop ? "true" : "false",
			caps->has_rc_options ? "true" : "false", caps->has_pskip ? "true" : "false",
			caps->has_srd ? "true" : "false", caps->has_max_pic_size ? "true" : "false",
			caps->has_super_frame ? "true" : "false",
			caps->has_color2grey ? "true" : "false", caps->has_roi ? "true" : "false",
			caps->has_map_roi ? "true" : "false",
			caps->has_qp_bounds_per_frame ? "true" : "false",
			caps->has_qpg_mode ? "true" : "false", caps->has_mbrc ? "true" : "false",
			caps->has_enc_denoise ? "true" : "false", caps->has_gdr ? "true" : "false",
			caps->has_h264_trans ? "true" : "false",
			caps->has_h265_trans ? "true" : "false",
			caps->has_enc_crop ? "true" : "false",
			caps->has_resize_mode ? "true" : "false",
			caps->has_jpeg_ql ? "true" : "false", caps->has_jpeg_qp ? "true" : "false");
		return 1;
	}

	(void)caps;
	return 0;
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
			rss_ctrl_resp_error(resp, resp_size, "need value");                        \
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
			rss_ctrl_resp_error(resp, resp_size, "need value");                        \
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
		rss_ctrl_resp(resp, resp_size,
			      "{\"status\":\"%s\",\"mode\":\"%s\",\"r_gain\":%u,\"b_gain\":%u}",
			      ret == 0 ? "ok" : "error", wb_mode_str(wb.mode), wb.r_gain,
			      wb.b_gain);
		return 1;
	}

	if (strstr(cmd_json, "\"get-wb\"")) {
		rss_wb_config_t wb = {0};
		RSS_HAL_CALL(st->ops, isp_get_wb, st->hal_ctx, &wb);
		rss_ctrl_resp(resp, resp_size,
			      "{\"status\":\"ok\",\"mode\":\"%s\",\"r_gain\":%u,\"g_gain\":%u,"
			      "\"b_gain\":%u}",
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
		rss_ctrl_resp(resp, resp_size,
			      "{\"status\":\"ok\","
			      "\"brightness\":%u,\"contrast\":%u,\"saturation\":%u,"
			      "\"sharpness\":%u,\"hue\":%u,\"sinter\":%u,\"temper\":%u,"
			      "\"hflip\":%d,\"vflip\":%d,\"ae_comp\":%d,"
			      "\"max_again\":%u,\"max_dgain\":%u,"
			      "\"wb_mode\":\"%s\",\"wb_r\":%u,\"wb_g\":%u,\"wb_b\":%u}",
			      bri, con, sat, shp, hue, sin, tem, hf, vf, ae, again, dgain,
			      wb.mode == RSS_WB_MANUAL ? "manual" : "auto", wb.r_gain, wb.g_gain,
			      wb.b_gain);
		return 1;
	}

	if (strstr(cmd_json, "\"get-exposure\"")) {
		rss_exposure_t exp = {0};
		RSS_HAL_CALL(st->ops, isp_get_exposure, st->hal_ctx, &exp);
		rss_ctrl_resp(resp, resp_size,
			      "{\"total_gain\":%u,\"exposure_us\":%u,\"ae_luma\":%u}",
			      exp.total_gain, exp.exposure_time, exp.ae_luma);
		return 1;
	}

	if (strstr(cmd_json, "\"set-running-mode\"")) {
		char mode_str[8];
		if (rss_json_get_str(cmd_json, "value", mode_str, sizeof(mode_str)) == 0) {
			int mode = (strcmp(mode_str, "night") == 0) ? 1 : 0;
			RSS_HAL_CALL(st->ops, isp_set_running_mode, st->hal_ctx, mode);
			rss_ctrl_resp(resp, resp_size, "{\"status\":\"ok\",\"mode\":\"%s\"}",
				      mode ? "night" : "day");
		} else {
			rss_ctrl_resp(resp, resp_size, "{\"status\":\"error\"}");
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
		int persons = st->ivs_persondet ? atomic_load(&st->ivs_person_count) : -1;
		rss_ctrl_resp(resp, resp_size,
			      "{\"status\":\"ok\",\"active\":%s,\"motion\":%s,"
			      "\"persondet\":%s,\"persons\":%d,\"timestamp\":%" PRId64 "}",
			      st->ivs_active ? "true" : "false", motion ? "true" : "false",
			      st->ivs_persondet ? "true" : "false", persons, ts);
		return 1;
	}

	if (strstr(cmd_json, "\"ivs-detections\"")) {
		if (!st->ivs_active) {
			rss_ctrl_resp_error(resp, resp_size, "ivs not active");
			return 1;
		}
		pthread_mutex_lock(&st->ivs_det_lock);
		int count = st->ivs_detections.count;
		int off = snprintf(resp, resp_size,
				   "{\"status\":\"ok\",\"count\":%d,\"detections\":[", count);
		for (int i = 0; i < count && off < resp_size - 64; i++) {
			rss_ivs_detection_t *d = &st->ivs_detections.detections[i];
			off += snprintf(resp + off, resp_size - off,
					"%s{\"x0\":%d,\"y0\":%d,\"x1\":%d,\"y1\":%d,"
					"\"confidence\":%.2f,\"class\":%d}",
					i > 0 ? "," : "", d->box.p0_x, d->box.p0_y, d->box.p1_x,
					d->box.p1_y, (double)d->confidence, d->class_id);
		}
		pthread_mutex_unlock(&st->ivs_det_lock);
		snprintf(resp + off, resp_size - off, "]}");
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
			rss_ctrl_resp(resp, resp_size, "{\"status\":\"ok\",\"sensitivity\":%d}",
				      sens);
		} else {
			rss_ctrl_resp_error(resp, resp_size, "invalid or ivs not active");
		}
		return 1;
	}

	if (strstr(cmd_json, "\"ivs-set-skip-frames\"")) {
		int val = -1;
		if (rss_json_get_int(cmd_json, "value", &val) == 0 && st->ivs_active && val >= 0) {
			rss_ivs_move_param_t mp;
			memset(&mp, 0, sizeof(mp));
			if (RSS_HAL_CALL(st->ops, ivs_get_param, st->hal_ctx, st->ivs_chn, &mp) ==
			    0) {
				mp.skip_frame_count = val;
				RSS_HAL_CALL(st->ops, ivs_set_param, st->hal_ctx, st->ivs_chn, &mp);
			}
			rss_ctrl_resp(resp, resp_size, "{\"status\":\"ok\",\"skip_frames\":%d}",
				      val);
		} else {
			rss_ctrl_resp_error(resp, resp_size, "invalid or ivs not active");
		}
		return 1;
	}

	return 0;
}

/* ── Pipeline lifecycle commands ── */

/* Find the JPEG stream paired with a video stream (same FS channel) */
static int find_jpeg_for_video_ctrl(rvd_state_t *st, int video_idx)
{
	int target_fs = st->streams[video_idx].fs_chn;
	for (int j = 0; j < st->jpeg_count; j++) {
		int ji = st->jpeg_streams[j];
		if (ji >= 0 && st->streams[ji].fs_chn == target_fs)
			return ji;
	}
	return -1;
}

/*
 * Core restart sequence for a video stream + its paired JPEG.
 * Called after config has been updated. Returns 0 on success.
 */
static int do_stream_restart(rvd_state_t *st, int chn, char *resp, int resp_size)
{
	int jpeg = find_jpeg_for_video_ctrl(st, chn);
	bool has_ivs = (st->streams[chn].fs_chn == 1 && st->ivs_active);

	RSS_INFO("stream-restart: channel %d (jpeg=%d ivs=%d)", chn, jpeg, has_ivs);

	/* Stop: IVS → JPEG → video */
	if (has_ivs) {
		if (st->ivs_tid) {
			pthread_join(st->ivs_tid, NULL);
			st->ivs_tid = 0;
		}
		rvd_ivs_stop(st);
	}
	if (jpeg >= 0)
		rvd_stream_stop(st, jpeg);
	rvd_stream_stop(st, chn);

	/* Deinit: JPEG → video (unregister before group destroy) */
	if (jpeg >= 0)
		rvd_stream_deinit(st, jpeg);
	rvd_stream_deinit(st, chn);
	if (has_ivs)
		rvd_ivs_deinit(st);

	/* Reinit: IVS → video → JPEG */
	if (has_ivs)
		rvd_ivs_init(st);

	int ret = rvd_stream_init(st, chn);
	if (ret != RSS_OK) {
		RSS_ERROR("stream-restart: init stream %d failed: %d", chn, ret);
		if (resp)
			rss_ctrl_resp_error(resp, resp_size, "init failed");
		return -1;
	}
	if (jpeg >= 0) {
		/* JPEG inherits parent video's resolution */
		st->streams[jpeg].enc_cfg.width = st->streams[chn].enc_cfg.width;
		st->streams[jpeg].enc_cfg.height = st->streams[chn].enc_cfg.height;
		ret = rvd_stream_init(st, jpeg);
		if (ret != RSS_OK)
			RSS_WARN("stream-restart: jpeg init failed: %d (non-fatal)", ret);
	}

	/* Start: video → JPEG → IVS */
	if (has_ivs)
		rvd_ivs_start(st);

	rvd_stream_start(st, chn);
	if (jpeg >= 0 && st->streams[jpeg].ring)
		rvd_stream_start(st, jpeg);

	/* IVS thread relaunch */
	if (has_ivs) {
		pthread_attr_t ivs_attr;
		pthread_attr_init(&ivs_attr);
		pthread_attr_setstacksize(&ivs_attr, 64 * 1024);
		pthread_create(&st->ivs_tid, &ivs_attr, rvd_ivs_thread, st);
		pthread_attr_destroy(&ivs_attr);
	}

	RSS_INFO("stream-restart: channel %d complete", chn);
	return 0;
}

static int validate_video_channel(rvd_state_t *st, const char *cmd_json, int *out_chn, char *resp,
				  int resp_size)
{
	if (rss_json_get_int(cmd_json, "channel", out_chn) != 0 || *out_chn < 0 ||
	    *out_chn >= st->stream_count || st->streams[*out_chn].is_jpeg) {
		rss_ctrl_resp_error(resp, resp_size, "need valid video channel");
		return -1;
	}
	return 0;
}

static int handle_pipeline_cmd(const char *cmd_json, rvd_state_t *st, char *resp, int resp_size)
{
	int chn;

	if (strstr(cmd_json, "\"stream-restart\"")) {
		if (validate_video_channel(st, cmd_json, &chn, resp, resp_size) < 0)
			return 1;
		if (do_stream_restart(st, chn, resp, resp_size) < 0)
			return 1;
		rss_ctrl_resp_ok(resp, resp_size);
		return 1;
	}

	if (strstr(cmd_json, "\"set-codec\"")) {
		char val[8];
		if (validate_video_channel(st, cmd_json, &chn, resp, resp_size) < 0)
			return 1;
		if (rss_json_get_str(cmd_json, "value", val, sizeof(val)) != 0) {
			rss_ctrl_resp_error(resp, resp_size, "need value (h264|h265)");
			return 1;
		}

		rss_codec_t codec;
		if (strcasecmp(val, "h265") == 0 || strcasecmp(val, "hevc") == 0)
			codec = RSS_CODEC_H265;
		else if (strcasecmp(val, "h264") == 0 || strcasecmp(val, "avc") == 0)
			codec = RSS_CODEC_H264;
		else {
			rss_ctrl_resp_error(resp, resp_size, "unknown codec");
			return 1;
		}

		/* Check H.265 capability */
		const rss_hal_caps_t *caps =
			st->ops->get_caps ? st->ops->get_caps(st->hal_ctx) : NULL;
		if (codec == RSS_CODEC_H265 && caps && !caps->has_h265) {
			rss_ctrl_resp_error(resp, resp_size, "h265 not supported");
			return 1;
		}

		if (st->streams[chn].enc_cfg.codec == codec) {
			rss_ctrl_resp_ok(resp, resp_size); /* already set */
			return 1;
		}

		/* Save old codec, update, restart, restore on failure */
		rss_codec_t old_codec = st->streams[chn].enc_cfg.codec;
		st->streams[chn].enc_cfg.codec = codec;
		RSS_INFO("set-codec: channel %d → %s", chn, val);

		if (do_stream_restart(st, chn, resp, resp_size) < 0) {
			st->streams[chn].enc_cfg.codec = old_codec;
			return 1;
		}
		rss_config_set_str(st->cfg, st->streams[chn].cfg_sect, "codec", val);
		rss_ctrl_resp_ok(resp, resp_size);
		return 1;
	}

	if (strstr(cmd_json, "\"set-resolution\"")) {
		int w, h;
		if (validate_video_channel(st, cmd_json, &chn, resp, resp_size) < 0)
			return 1;
		if (rss_json_get_int(cmd_json, "width", &w) != 0 ||
		    rss_json_get_int(cmd_json, "height", &h) != 0 || w < 32 || h < 32 || w > 4096 ||
		    h > 4096) {
			rss_ctrl_resp_error(resp, resp_size, "need width and height (32-4096)");
			return 1;
		}

		/* Even dimensions required by encoder */
		w = (w + 1) & ~1;
		h = (h + 1) & ~1;

		if ((int)st->streams[chn].enc_cfg.width == w &&
		    (int)st->streams[chn].enc_cfg.height == h) {
			rss_ctrl_resp_ok(resp, resp_size);
			return 1;
		}

		/* Save old config for rollback */
		int old_enc_w = st->streams[chn].enc_cfg.width;
		int old_enc_h = st->streams[chn].enc_cfg.height;
		int old_fs_w = st->streams[chn].fs_cfg.width;
		int old_fs_h = st->streams[chn].fs_cfg.height;
		int old_sc_w = st->streams[chn].fs_cfg.scaler.out_width;
		int old_sc_h = st->streams[chn].fs_cfg.scaler.out_height;

		/* Update encoder + framesource config */
		st->streams[chn].enc_cfg.width = w;
		st->streams[chn].enc_cfg.height = h;
		st->streams[chn].fs_cfg.width = w;
		st->streams[chn].fs_cfg.height = h;
		if (st->streams[chn].fs_cfg.scaler.enable) {
			st->streams[chn].fs_cfg.scaler.out_width = w;
			st->streams[chn].fs_cfg.scaler.out_height = h;
		}
		RSS_INFO("set-resolution: channel %d → %dx%d", chn, w, h);

		/* Reconfigure FS scaler while channel is disabled.
		 * do_stream_restart stops+deinits first, but we need FS
		 * reconfigured before reinit. Do it manually. */
		int jpeg = find_jpeg_for_video_ctrl(st, chn);
		bool has_ivs = (st->streams[chn].fs_chn == 1 && st->ivs_active);

		/* Stop */
		if (has_ivs) {
			if (st->ivs_tid) {
				pthread_join(st->ivs_tid, NULL);
				st->ivs_tid = 0;
			}
			rvd_ivs_stop(st);
		}
		if (jpeg >= 0)
			rvd_stream_stop(st, jpeg);
		rvd_stream_stop(st, chn);

		/* Deinit */
		if (jpeg >= 0)
			rvd_stream_deinit(st, jpeg);
		rvd_stream_deinit(st, chn);
		if (has_ivs)
			rvd_ivs_deinit(st);

		/* Reconfigure FS scaler (channel disabled, not destroyed) */
		RSS_HAL_CALL(st->ops, fs_set_channel_attr, st->hal_ctx, st->streams[chn].fs_chn,
			     &st->streams[chn].fs_cfg);

		/* Reinit */
		if (has_ivs)
			rvd_ivs_init(st);

		int ret = rvd_stream_init(st, chn);
		if (ret != RSS_OK) {
			RSS_ERROR("set-resolution: init failed: %d, restoring old config", ret);
			st->streams[chn].enc_cfg.width = old_enc_w;
			st->streams[chn].enc_cfg.height = old_enc_h;
			st->streams[chn].fs_cfg.width = old_fs_w;
			st->streams[chn].fs_cfg.height = old_fs_h;
			st->streams[chn].fs_cfg.scaler.out_width = old_sc_w;
			st->streams[chn].fs_cfg.scaler.out_height = old_sc_h;
			rss_ctrl_resp_error(resp, resp_size, "init failed");
			return 1;
		}
		if (jpeg >= 0) {
			st->streams[jpeg].enc_cfg.width = w;
			st->streams[jpeg].enc_cfg.height = h;
			ret = rvd_stream_init(st, jpeg);
			if (ret != RSS_OK)
				RSS_WARN("set-resolution: jpeg init failed: %d", ret);
		}

		/* Start */
		if (has_ivs)
			rvd_ivs_start(st);
		rvd_stream_start(st, chn);
		if (jpeg >= 0 && st->streams[jpeg].ring)
			rvd_stream_start(st, jpeg);
		if (has_ivs) {
			pthread_attr_t ivs_attr;
			pthread_attr_init(&ivs_attr);
			pthread_attr_setstacksize(&ivs_attr, 64 * 1024);
			pthread_create(&st->ivs_tid, &ivs_attr, rvd_ivs_thread, st);
			pthread_attr_destroy(&ivs_attr);
		}

		/* Persist config only after successful restart */
		rss_config_set_int(st->cfg, st->streams[chn].cfg_sect, "width", w);
		rss_config_set_int(st->cfg, st->streams[chn].cfg_sect, "height", h);
		RSS_INFO("set-resolution: channel %d → %dx%d complete", chn, w, h);
		rss_ctrl_resp_ok(resp, resp_size);
		return 1;
	}

	if (strstr(cmd_json, "\"osd-restart\"")) {
		int pool_kb = 0;
		int font_size = 0;
		rss_json_get_int(cmd_json, "pool_kb", &pool_kb);
		rss_json_get_int(cmd_json, "font_size", &font_size);

		/* Update font_size in running config so rvd_osd_init_stream
		 * calculates correct region dimensions on reinit. */
		if (font_size > 0) {
			rss_config_set_int(st->cfg, "osd", "font_size", font_size);
			RSS_INFO("osd-restart: font_size=%d", font_size);
		}

		RSS_INFO("osd-restart: stopping all streams (pool_kb=%d)", pool_kb);

		/* Count video streams (non-JPEG) */
		int video_count = 0;
		int video_indices[RVD_MAX_STREAMS];
		int jpeg_indices[RVD_MAX_STREAMS];
		int jpeg_count_local = 0;
		for (int i = 0; i < st->stream_count; i++) {
			if (st->streams[i].is_jpeg)
				jpeg_indices[jpeg_count_local++] = i;
			else
				video_indices[video_count++] = i;
		}

		/* IVS: stop before streams (IVS is bound to sub-stream) */
		bool has_ivs = false;
		for (int j = 0; j < video_count; j++) {
			if (st->streams[video_indices[j]].fs_chn == 1 && st->ivs_active) {
				has_ivs = true;
				break;
			}
		}
		if (has_ivs) {
			if (st->ivs_tid) {
				pthread_join(st->ivs_tid, NULL);
				st->ivs_tid = 0;
			}
			rvd_ivs_stop(st);
		}

		/* Stop all: JPEG first, then video (reverse of start order) */
		for (int j = jpeg_count_local - 1; j >= 0; j--)
			rvd_stream_stop(st, jpeg_indices[j]);
		for (int j = video_count - 1; j >= 0; j--)
			rvd_stream_stop(st, video_indices[j]);

		/* Deinit all: JPEG first (unregister from group before destroy) */
		for (int j = jpeg_count_local - 1; j >= 0; j--)
			rvd_stream_deinit(st, jpeg_indices[j]);
		for (int j = video_count - 1; j >= 0; j--)
			rvd_stream_deinit(st, video_indices[j]);
		if (has_ivs)
			rvd_ivs_deinit(st);

		/* All OSD groups destroyed. Try resizing the pool. */
		if (pool_kb > 0) {
			uint32_t new_pool = (uint32_t)pool_kb * 1024;
			RSS_INFO("osd-restart: resizing OSD pool to %u KB", pool_kb);
			int ret = RSS_HAL_CALL(st->ops, osd_set_pool_size, st->hal_ctx, new_pool);
			RSS_INFO("osd-restart: osd_set_pool_size returned %d", ret);
		}

		/* IVS reinit before streams (SDK requires group created before bind) */
		if (has_ivs)
			rvd_ivs_init(st);

		/* Reinit all: video first, then JPEG */
		bool init_ok[RVD_MAX_STREAMS] = {0};
		for (int j = 0; j < video_count; j++) {
			int ret = rvd_stream_init(st, video_indices[j]);
			if (ret != RSS_OK)
				RSS_ERROR("osd-restart: stream%d init failed: %d", video_indices[j],
					  ret);
			else
				init_ok[video_indices[j]] = true;
		}
		for (int j = 0; j < jpeg_count_local; j++) {
			int ret = rvd_stream_init(st, jpeg_indices[j]);
			if (ret != RSS_OK)
				RSS_WARN("osd-restart: jpeg stream%d init failed: %d",
					 jpeg_indices[j], ret);
			else
				init_ok[jpeg_indices[j]] = true;
		}

		/* IVS start after streams are ready */
		if (has_ivs)
			rvd_ivs_start(st);

		/* Start all: video first, then JPEG (skip failed inits) */
		for (int j = 0; j < video_count; j++) {
			if (init_ok[video_indices[j]])
				rvd_stream_start(st, video_indices[j]);
		}
		for (int j = 0; j < jpeg_count_local; j++) {
			if (init_ok[jpeg_indices[j]])
				rvd_stream_start(st, jpeg_indices[j]);
		}

		/* IVS thread relaunch */
		if (has_ivs) {
			pthread_attr_t ivs_attr;
			pthread_attr_init(&ivs_attr);
			pthread_attr_setstacksize(&ivs_attr, 64 * 1024);
			pthread_create(&st->ivs_tid, &ivs_attr, rvd_ivs_thread, st);
			pthread_attr_destroy(&ivs_attr);
		}

		RSS_INFO("osd-restart: complete");
		rss_ctrl_resp_ok(resp, resp_size);
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
		int channel = -1;
		rss_json_get_int(cmd_json, "channel", &channel);

		bool enable;
		if (rss_json_get_str(cmd_json, "value", val_str, sizeof(val_str)) == 0)
			enable = (strcmp(val_str, "on") == 0 || strcmp(val_str, "1") == 0);
		else if (channel >= 0 && channel < st->stream_count)
			enable = !st->privacy[channel];
		else
			enable = !st->privacy[0];

		rvd_osd_set_privacy(st, enable, channel);

		int n = snprintf(resp, resp_size, "{\"status\":\"ok\",\"privacy\":[");
		int first = 1;
		for (int i = 0; i < st->stream_count; i++) {
			if (st->streams[i].is_jpeg)
				continue;
			if (n >= resp_size - 2)
				break;
			n += snprintf(resp + n, resp_size - n, "%s\"%s\"", first ? "" : ",",
				      st->privacy[i] ? "on" : "off");
			first = 0;
		}
		if (n < resp_size - 2)
			snprintf(resp + n, resp_size - n, "]}");
		else
			resp[resp_size - 1] = '\0';
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

	if (handle_encoder_advanced_cmd(cmd_json, st, resp_buf, resp_buf_size))
		CTRL_RETURN(resp_buf);

	if (handle_isp_cmd(cmd_json, st, resp_buf, resp_buf_size))
		CTRL_RETURN(resp_buf);

	if (handle_ivs_cmd(cmd_json, st, resp_buf, resp_buf_size))
		CTRL_RETURN(resp_buf);

	if (handle_pipeline_cmd(cmd_json, st, resp_buf, resp_buf_size))
		CTRL_RETURN(resp_buf);

	if (handle_config_cmd(cmd_json, st, resp_buf, resp_buf_size))
		CTRL_RETURN(resp_buf);

	return rss_ctrl_resp_error(resp_buf, resp_buf_size, "unknown command");
}

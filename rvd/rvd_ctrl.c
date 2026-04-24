/*
 * rvd_ctrl.c -- Control socket command handler
 *
 * Handles raptorctl commands for encoder settings, ISP tuning,
 * config management, privacy mode, and IVS status.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
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
 * Sub-handler return convention:
 *   > 0  = handled, return value is response byte count
 *   0    = not my command, try next handler
 *   < 0  = error
 */

/* Format HAL return code into JSON error response */
static int fmt_hal_result(char *buf, int bufsz, int ret)
{
	if (ret == 0)
		return rss_ctrl_resp_ok(buf, bufsz);
	else if (ret == RSS_ERR_NOTSUP)
		return rss_ctrl_resp_error(buf, bufsz, "not supported on this SoC");
	else {
		char reason[32];
		snprintf(reason, sizeof(reason), "failed (%d)", ret);
		return rss_ctrl_resp_error(buf, bufsz, reason);
	}
}

/* ── Encoder commands ── */

static int handle_encoder_cmd(const char *cmd, const char *cmd_json, rvd_state_t *st, char *resp,
			      int resp_size)
{
	int chn, val, val2;

	if (strcmp(cmd, "request-idr") == 0) {
		int target = -1;
		rss_json_get_int(cmd_json, "channel", &target);
		for (int i = 0; i < st->stream_count; i++) {
			if (target >= 0 && i != target)
				continue;
			RSS_HAL_CALL(st->ops, enc_request_idr, st->hal_ctx, st->streams[i].chn);
		}
		return rss_ctrl_resp_ok(resp, resp_size);
	}

	if (strcmp(cmd, "set-rc-mode") == 0) {
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
			cJSON *r = cJSON_CreateObject();
			cJSON_AddStringToObject(r, "status", ret == 0 ? "ok" : "error");
			cJSON_AddStringToObject(r, "rc_mode", mode_str);
			return rss_ctrl_resp_json(resp, resp_size, r);
		} else {
			return rss_ctrl_resp_error(resp, resp_size, "need channel and mode");
		}
	}

	if (strcmp(cmd, "set-bitrate") == 0) {
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
			return fmt_hal_result(resp, resp_size, ret);
		}
		return rss_ctrl_resp_error(resp, resp_size, "need channel and value");
	}

	if (strcmp(cmd, "set-gop") == 0) {
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 &&
		    rss_json_get_int(cmd_json, "value", &val) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			int ret = RSS_HAL_CALL(st->ops, enc_set_gop, st->hal_ctx,
					       st->streams[chn].chn, val);
			if (ret == 0) {
				st->streams[chn].enc_cfg.gop_length = val;
				rss_config_set_int(st->cfg, st->streams[chn].cfg_sect, "gop", val);
			}
			return fmt_hal_result(resp, resp_size, ret);
		}
		return rss_ctrl_resp_error(resp, resp_size, "need channel and value");
	}

	if (strcmp(cmd, "set-fps") == 0) {
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 &&
		    rss_json_get_int(cmd_json, "value", &val) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			int ret = RSS_HAL_CALL(st->ops, enc_set_fps, st->hal_ctx,
					       st->streams[chn].chn, val, 1);
			if (ret == 0) {
				st->streams[chn].enc_cfg.fps_num = val;
				rss_config_set_int(st->cfg, st->streams[chn].cfg_sect, "fps", val);
			}
			return fmt_hal_result(resp, resp_size, ret);
		}
		return rss_ctrl_resp_error(resp, resp_size, "need channel and value");
	}

	if (strcmp(cmd, "set-qp-bounds") == 0) {
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
			return fmt_hal_result(resp, resp_size, ret);
		}
		return rss_ctrl_resp_error(resp, resp_size, "need channel, min, max");
	}

	if (strcmp(cmd, "get-bitrate") == 0) {
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			uint32_t avg = 0;
			RSS_HAL_CALL(st->ops, enc_get_avg_bitrate, st->hal_ctx,
				     st->streams[chn].chn, &avg);
			cJSON *r = cJSON_CreateObject();
			cJSON_AddStringToObject(r, "status", "ok");
			cJSON_AddNumberToObject(r, "bitrate",
						(double)st->streams[chn].enc_cfg.bitrate);
			cJSON_AddNumberToObject(r, "avg_bitrate", (double)avg);
			return rss_ctrl_resp_json(resp, resp_size, r);
		}
		return rss_ctrl_resp_error(resp, resp_size, "need channel");
	}

	if (strcmp(cmd, "get-fps") == 0) {
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			uint32_t num = 0, den = 0;
			RSS_HAL_CALL(st->ops, enc_get_fps, st->hal_ctx, st->streams[chn].chn, &num,
				     &den);
			cJSON *r = cJSON_CreateObject();
			cJSON_AddStringToObject(r, "status", "ok");
			cJSON_AddNumberToObject(r, "fps_num", (double)num);
			cJSON_AddNumberToObject(r, "fps_den", (double)den);
			return rss_ctrl_resp_json(resp, resp_size, r);
		}
		return rss_ctrl_resp_error(resp, resp_size, "need channel");
	}

	if (strcmp(cmd, "get-gop") == 0) {
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			uint32_t gop = 0;
			RSS_HAL_CALL(st->ops, enc_get_gop_attr, st->hal_ctx, st->streams[chn].chn,
				     &gop);
			cJSON *r = cJSON_CreateObject();
			cJSON_AddStringToObject(r, "status", "ok");
			cJSON_AddNumberToObject(r, "gop", (double)gop);
			return rss_ctrl_resp_json(resp, resp_size, r);
		}
		return rss_ctrl_resp_error(resp, resp_size, "need channel");
	}

	if (strcmp(cmd, "get-qp-bounds") == 0) {
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			cJSON *r = cJSON_CreateObject();
			cJSON_AddStringToObject(r, "status", "ok");
			cJSON_AddNumberToObject(r, "min_qp",
						(double)st->streams[chn].enc_cfg.min_qp);
			cJSON_AddNumberToObject(r, "max_qp",
						(double)st->streams[chn].enc_cfg.max_qp);
			return rss_ctrl_resp_json(resp, resp_size, r);
		}
		return rss_ctrl_resp_error(resp, resp_size, "need channel");
	}

	if (strcmp(cmd, "get-rc-mode") == 0) {
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			static const char *rc_names[] = {"fixqp", "cbr",	"vbr",
							 "smart", "capped_vbr", "capped_quality"};
			int mode = st->streams[chn].enc_cfg.rc_mode;
			const char *name =
				(mode >= 0 && mode < (int)(sizeof(rc_names) / sizeof(rc_names[0])))
					? rc_names[mode]
					: "unknown";
			cJSON *r = cJSON_CreateObject();
			cJSON_AddStringToObject(r, "status", "ok");
			cJSON_AddStringToObject(r, "rc_mode", name);
			cJSON_AddNumberToObject(r, "rc_mode_id", (double)mode);
			return rss_ctrl_resp_json(resp, resp_size, r);
		}
		return rss_ctrl_resp_error(resp, resp_size, "need channel");
	}

	return 0; /* not an encoder command */
}

/* ── Advanced encoder commands ── */

static int handle_encoder_advanced_cmd(const char *cmd, const char *cmd_json, rvd_state_t *st,
				       char *resp, int resp_size)
{
	int chn, val, val2;
	const rss_hal_caps_t *caps = st->ops->get_caps ? st->ops->get_caps(st->hal_ctx) : NULL;

	/* ── Table-driven encoder params (enc-set / enc-get / enc-list) ── */

	typedef enum { EP_INT, EP_U32, EP_BOOL } enc_param_type_t;

	typedef struct {
		const char *name;
		enc_param_type_t type;
		size_t set_off;
		size_t get_off;
	} enc_param_t;

	/* Typed function pointer signatures for the generic dispatch */
	typedef int (*enc_set_int_fn)(void *, int, int);
	typedef int (*enc_set_u32_fn)(void *, int, uint32_t);
	typedef int (*enc_set_bool_fn)(void *, int, bool);
	typedef int (*enc_get_int_fn)(void *, int, int *);
	typedef int (*enc_get_u32_fn)(void *, int, uint32_t *);
	typedef int (*enc_get_bool_fn)(void *, int, bool *);

#define EP_OFF(field) offsetof(rss_hal_ops_t, field)

	static const enc_param_t enc_params[] = {
		{"qp", EP_INT, EP_OFF(enc_set_qp), 0},
		{"qp_ip_delta", EP_INT, EP_OFF(enc_set_qp_ip_delta), 0},
		{"qp_pb_delta", EP_INT, EP_OFF(enc_set_qp_pb_delta), 0},
		{"max_psnr", EP_INT, EP_OFF(enc_set_max_psnr), 0},
		{"gop_mode", EP_INT, EP_OFF(enc_set_gop_mode), EP_OFF(enc_get_gop_mode)},
		{"rc_options", EP_U32, EP_OFF(enc_set_rc_options), EP_OFF(enc_get_rc_options)},
		{"max_same_scene", EP_U32, EP_OFF(enc_set_max_same_scene_cnt),
		 EP_OFF(enc_get_max_same_scene_cnt)},
		{"qpg_mode", EP_INT, EP_OFF(enc_set_qpg_mode), EP_OFF(enc_get_qpg_mode)},
		{"entropy_mode", EP_INT, EP_OFF(enc_set_chn_entropy_mode), 0},
		{"stream_buf_size", EP_U32, EP_OFF(enc_set_stream_buf_size),
		 EP_OFF(enc_get_stream_buf_size)},
		{"jpeg_qp", EP_INT, EP_OFF(enc_set_jpeg_qp), EP_OFF(enc_get_jpeg_qp)},
		{"color2grey", EP_BOOL, EP_OFF(enc_set_color2grey), EP_OFF(enc_get_color2grey)},
		{"mbrc", EP_BOOL, EP_OFF(enc_set_mbrc), EP_OFF(enc_get_mbrc)},
		{"resize_mode", EP_INT, EP_OFF(enc_set_resize_mode), 0},
	};

#undef EP_OFF

#define ENC_PARAM_COUNT (sizeof(enc_params) / sizeof(enc_params[0]))

	static const char *ep_type_str[] = {"int", "uint", "bool"};

	if (strcmp(cmd, "enc-set") == 0) {
		char param[32] = "";
		rss_json_get_str(cmd_json, "param", param, sizeof(param));
		if (rss_json_get_int(cmd_json, "channel", &chn) != 0 || chn < 0 ||
		    chn >= st->stream_count || !param[0])
			return rss_ctrl_resp_error(resp, resp_size, "need channel, param, value");
		if (rss_json_get_int(cmd_json, "value", &val) != 0)
			return rss_ctrl_resp_error(resp, resp_size, "need value");
		for (unsigned i = 0; i < ENC_PARAM_COUNT; i++) {
			if (strcmp(param, enc_params[i].name) != 0)
				continue;
			if (enc_params[i].set_off == 0)
				return rss_ctrl_resp_error(resp, resp_size, "no setter");
			void *fn = *(void **)((char *)st->ops + enc_params[i].set_off);
			if (!fn)
				return rss_ctrl_resp_error(resp, resp_size,
							   "not supported on this SoC");
			int ret;
			int hw_chn = st->streams[chn].chn;
			switch (enc_params[i].type) {
			case EP_INT:
				ret = ((enc_set_int_fn)fn)(st->hal_ctx, hw_chn, val);
				break;
			case EP_U32:
				ret = ((enc_set_u32_fn)fn)(st->hal_ctx, hw_chn, (uint32_t)val);
				break;
			case EP_BOOL:
				ret = ((enc_set_bool_fn)fn)(st->hal_ctx, hw_chn, (bool)val);
				break;
			default:
				ret = RSS_ERR;
				break;
			}
			return fmt_hal_result(resp, resp_size, ret);
		}
		return rss_ctrl_resp_error(resp, resp_size, "unknown param");
	}

	if (strcmp(cmd, "enc-get") == 0) {
		char param[32] = "";
		rss_json_get_str(cmd_json, "param", param, sizeof(param));
		if (rss_json_get_int(cmd_json, "channel", &chn) != 0 || chn < 0 ||
		    chn >= st->stream_count || !param[0])
			return rss_ctrl_resp_error(resp, resp_size, "need channel and param");
		for (unsigned i = 0; i < ENC_PARAM_COUNT; i++) {
			if (strcmp(param, enc_params[i].name) != 0)
				continue;
			if (enc_params[i].get_off == 0)
				return rss_ctrl_resp_error(resp, resp_size, "no getter");
			void *fn = *(void **)((char *)st->ops + enc_params[i].get_off);
			if (!fn)
				return rss_ctrl_resp_error(resp, resp_size,
							   "not supported on this SoC");
			int hw_chn = st->streams[chn].chn;
			int ret;
			cJSON *r = cJSON_CreateObject();
			if (!r)
				return rss_ctrl_resp_error(resp, resp_size, "alloc");
			switch (enc_params[i].type) {
			case EP_INT: {
				int out = 0;
				ret = ((enc_get_int_fn)fn)(st->hal_ctx, hw_chn, &out);
				if (ret == 0) {
					cJSON_AddStringToObject(r, "status", "ok");
					cJSON_AddStringToObject(r, "param", param);
					cJSON_AddNumberToObject(r, "value", (double)out);
					return rss_ctrl_resp_json(resp, resp_size, r);
				}
				break;
			}
			case EP_U32: {
				uint32_t out = 0;
				ret = ((enc_get_u32_fn)fn)(st->hal_ctx, hw_chn, &out);
				if (ret == 0) {
					cJSON_AddStringToObject(r, "status", "ok");
					cJSON_AddStringToObject(r, "param", param);
					cJSON_AddNumberToObject(r, "value", (double)out);
					return rss_ctrl_resp_json(resp, resp_size, r);
				}
				break;
			}
			case EP_BOOL: {
				bool out = false;
				ret = ((enc_get_bool_fn)fn)(st->hal_ctx, hw_chn, &out);
				if (ret == 0) {
					cJSON_AddStringToObject(r, "status", "ok");
					cJSON_AddStringToObject(r, "param", param);
					cJSON_AddBoolToObject(r, "value", out);
					return rss_ctrl_resp_json(resp, resp_size, r);
				}
				break;
			}
			default:
				ret = RSS_ERR;
				break;
			}
			cJSON_Delete(r);
			return fmt_hal_result(resp, resp_size, ret);
		}
		return rss_ctrl_resp_error(resp, resp_size, "unknown param");
	}

	if (strcmp(cmd, "enc-list") == 0) {
		int list_chn = -1;
		rss_json_get_int(cmd_json, "channel", &list_chn);
		int hw_list_chn = -1;
		if (list_chn >= 0 && list_chn < st->stream_count)
			hw_list_chn = st->streams[list_chn].chn;

		cJSON *r = cJSON_CreateObject();
		if (!r)
			return rss_ctrl_resp_error(resp, resp_size, "alloc");
		cJSON_AddStringToObject(r, "status", "ok");
		cJSON *arr = cJSON_AddArrayToObject(r, "params");
		for (unsigned i = 0; i < ENC_PARAM_COUNT; i++) {
			const enc_param_t *p = &enc_params[i];
			cJSON *obj = cJSON_CreateObject();
			if (!obj)
				continue;
			cJSON_AddStringToObject(obj, "name", p->name);
			cJSON_AddStringToObject(obj, "type", ep_type_str[p->type]);
			bool has_set =
				p->set_off && *(void **)((char *)st->ops + p->set_off) != NULL;
			bool has_get =
				p->get_off && *(void **)((char *)st->ops + p->get_off) != NULL;
			cJSON_AddBoolToObject(obj, "set", has_set);
			cJSON_AddBoolToObject(obj, "get", has_get);

			if (hw_list_chn >= 0 && has_get) {
				void *fn = *(void **)((char *)st->ops + p->get_off);
				switch (p->type) {
				case EP_INT: {
					int out = 0;
					if (((enc_get_int_fn)fn)(st->hal_ctx, hw_list_chn, &out) ==
					    0)
						cJSON_AddNumberToObject(obj, "value", (double)out);
					break;
				}
				case EP_U32: {
					uint32_t out = 0;
					if (((enc_get_u32_fn)fn)(st->hal_ctx, hw_list_chn, &out) ==
					    0)
						cJSON_AddNumberToObject(obj, "value", (double)out);
					break;
				}
				case EP_BOOL: {
					bool out = false;
					if (((enc_get_bool_fn)fn)(st->hal_ctx, hw_list_chn, &out) ==
					    0)
						cJSON_AddBoolToObject(obj, "value", out);
					break;
				}
				default:
					break;
				}
			}

			cJSON_AddItemToArray(arr, obj);
		}
		return rss_ctrl_resp_json(resp, resp_size, r);
	}

#undef ENC_PARAM_COUNT

	/* ── QP bounds per frame (I/P separate) ── */
	if (strcmp(cmd, "set-qp-bounds-per-frame") == 0) {
		int min_i, max_i, min_p, max_p;
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 &&
		    rss_json_get_int(cmd_json, "min_i", &min_i) == 0 &&
		    rss_json_get_int(cmd_json, "max_i", &max_i) == 0 &&
		    rss_json_get_int(cmd_json, "min_p", &min_p) == 0 &&
		    rss_json_get_int(cmd_json, "max_p", &max_p) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			int ret = RSS_HAL_CALL(st->ops, enc_set_qp_bounds_per_frame, st->hal_ctx,
					       st->streams[chn].chn, min_i, max_i, min_p, max_p);
			return fmt_hal_result(resp, resp_size, ret);
		}
		return rss_ctrl_resp_error(resp, resp_size,
					   "need channel, min_i, max_i, min_p, max_p");
	}

	/* ── Max picture size ── */
	if (strcmp(cmd, "set-max-pic-size") == 0) {
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 &&
		    rss_json_get_int(cmd_json, "i_kbits", &val) == 0 &&
		    rss_json_get_int(cmd_json, "p_kbits", &val2) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			int ret = RSS_HAL_CALL(st->ops, enc_set_max_pic_size, st->hal_ctx,
					       st->streams[chn].chn, (uint32_t)val, (uint32_t)val2);
			return fmt_hal_result(resp, resp_size, ret);
		}
		return rss_ctrl_resp_error(resp, resp_size, "need channel, i_kbits, p_kbits");
	}

	/* ── H.264 transform ── */
	if (strcmp(cmd, "set-h264-trans") == 0) {
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 &&
		    rss_json_get_int(cmd_json, "value", &val) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			rss_enc_h264_trans_t cfg = {.chroma_qp_index_offset = val};
			int ret = RSS_HAL_CALL(st->ops, enc_set_h264_trans, st->hal_ctx,
					       st->streams[chn].chn, &cfg);
			return fmt_hal_result(resp, resp_size, ret);
		}
		return rss_ctrl_resp_error(resp, resp_size, "need channel and value");
	}

	if (strcmp(cmd, "get-h264-trans") == 0) {
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			rss_enc_h264_trans_t cfg = {0};
			int ret = RSS_HAL_CALL(st->ops, enc_get_h264_trans, st->hal_ctx,
					       st->streams[chn].chn, &cfg);
			if (ret == 0) {
				cJSON *r = cJSON_CreateObject();
				cJSON_AddStringToObject(r, "status", "ok");
				cJSON_AddNumberToObject(r, "chroma_qp_offset",
							(double)cfg.chroma_qp_index_offset);
				return rss_ctrl_resp_json(resp, resp_size, r);
			}
			return fmt_hal_result(resp, resp_size, ret);
		}
		return rss_ctrl_resp_error(resp, resp_size, "need channel");
	}

	/* ── H.265 transform ── */
	if (strcmp(cmd, "set-h265-trans") == 0) {
		int cr_off, cb_off;
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 &&
		    rss_json_get_int(cmd_json, "cr_offset", &cr_off) == 0 &&
		    rss_json_get_int(cmd_json, "cb_offset", &cb_off) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			rss_enc_h265_trans_t cfg = {.chroma_cr_qp_offset = cr_off,
						    .chroma_cb_qp_offset = cb_off};
			int ret = RSS_HAL_CALL(st->ops, enc_set_h265_trans, st->hal_ctx,
					       st->streams[chn].chn, &cfg);
			return fmt_hal_result(resp, resp_size, ret);
		}
		return rss_ctrl_resp_error(resp, resp_size, "need channel, cr_offset, cb_offset");
	}

	if (strcmp(cmd, "get-h265-trans") == 0) {
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			rss_enc_h265_trans_t cfg = {0};
			int ret = RSS_HAL_CALL(st->ops, enc_get_h265_trans, st->hal_ctx,
					       st->streams[chn].chn, &cfg);
			if (ret == 0) {
				cJSON *r = cJSON_CreateObject();
				cJSON_AddStringToObject(r, "status", "ok");
				cJSON_AddNumberToObject(r, "cr_offset",
							(double)cfg.chroma_cr_qp_offset);
				cJSON_AddNumberToObject(r, "cb_offset",
							(double)cfg.chroma_cb_qp_offset);
				return rss_ctrl_resp_json(resp, resp_size, r);
			}
			return fmt_hal_result(resp, resp_size, ret);
		}
		return rss_ctrl_resp_error(resp, resp_size, "need channel");
	}

	/* ── ROI ── */
	if (strcmp(cmd, "set-roi") == 0) {
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
			return fmt_hal_result(resp, resp_size, ret);
		}
		return rss_ctrl_resp_error(resp, resp_size,
					   "need channel, index, enable, x, y, w, h, qp");
	}

	if (strcmp(cmd, "get-roi") == 0) {
		int idx;
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 &&
		    rss_json_get_int(cmd_json, "index", &idx) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			rss_enc_roi_t roi = {0};
			int ret = RSS_HAL_CALL(st->ops, enc_get_roi, st->hal_ctx,
					       st->streams[chn].chn, (uint32_t)idx, &roi);
			if (ret == 0) {
				cJSON *r = cJSON_CreateObject();
				cJSON_AddStringToObject(r, "status", "ok");
				cJSON_AddNumberToObject(r, "index", (double)roi.index);
				cJSON_AddBoolToObject(r, "enable", roi.enable);
				cJSON_AddNumberToObject(r, "x", (double)roi.x);
				cJSON_AddNumberToObject(r, "y", (double)roi.y);
				cJSON_AddNumberToObject(r, "w", (double)roi.w);
				cJSON_AddNumberToObject(r, "h", (double)roi.h);
				cJSON_AddNumberToObject(r, "qp", (double)roi.qp);
				return rss_ctrl_resp_json(resp, resp_size, r);
			}
			return fmt_hal_result(resp, resp_size, ret);
		}
		return rss_ctrl_resp_error(resp, resp_size, "need channel and index");
	}

	/* ── Super frame ── */
	if (strcmp(cmd, "set-super-frame") == 0) {
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
			return fmt_hal_result(resp, resp_size, ret);
		}
		return rss_ctrl_resp_error(resp, resp_size, "need channel, mode, i_thr, p_thr");
	}

	if (strcmp(cmd, "get-super-frame") == 0) {
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			rss_super_frame_cfg_t cfg = {0};
			int ret = RSS_HAL_CALL(st->ops, enc_get_super_frame, st->hal_ctx,
					       st->streams[chn].chn, &cfg);
			if (ret == 0) {
				cJSON *r = cJSON_CreateObject();
				cJSON_AddStringToObject(r, "status", "ok");
				cJSON_AddNumberToObject(r, "mode", (double)cfg.mode);
				cJSON_AddNumberToObject(r, "i_thr", (double)cfg.i_bits_thr);
				cJSON_AddNumberToObject(r, "p_thr", (double)cfg.p_bits_thr);
				cJSON_AddNumberToObject(r, "priority", (double)cfg.priority);
				return rss_ctrl_resp_json(resp, resp_size, r);
			}
			return fmt_hal_result(resp, resp_size, ret);
		}
		return rss_ctrl_resp_error(resp, resp_size, "need channel");
	}

	/* ── P-skip ── */
	if (strcmp(cmd, "set-pskip") == 0) {
		int enable, max_frames;
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 &&
		    rss_json_get_int(cmd_json, "enable", &enable) == 0 &&
		    rss_json_get_int(cmd_json, "max_frames", &max_frames) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			rss_pskip_cfg_t cfg = {.enable = (bool)enable, .max_frames = max_frames};
			int ret = RSS_HAL_CALL(st->ops, enc_set_pskip, st->hal_ctx,
					       st->streams[chn].chn, &cfg);
			return fmt_hal_result(resp, resp_size, ret);
		}
		return rss_ctrl_resp_error(resp, resp_size, "need channel, enable, max_frames");
	}

	if (strcmp(cmd, "get-pskip") == 0) {
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			rss_pskip_cfg_t cfg = {0};
			int ret = RSS_HAL_CALL(st->ops, enc_get_pskip, st->hal_ctx,
					       st->streams[chn].chn, &cfg);
			if (ret == 0) {
				cJSON *r = cJSON_CreateObject();
				cJSON_AddStringToObject(r, "status", "ok");
				cJSON_AddBoolToObject(r, "enable", cfg.enable);
				cJSON_AddNumberToObject(r, "max_frames", (double)cfg.max_frames);
				return rss_ctrl_resp_json(resp, resp_size, r);
			}
			return fmt_hal_result(resp, resp_size, ret);
		}
		return rss_ctrl_resp_error(resp, resp_size, "need channel");
	}

	if (strcmp(cmd, "request-pskip") == 0) {
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			int ret = RSS_HAL_CALL(st->ops, enc_request_pskip, st->hal_ctx,
					       st->streams[chn].chn);
			return fmt_hal_result(resp, resp_size, ret);
		}
		return rss_ctrl_resp_error(resp, resp_size, "need channel");
	}

	/* ── SRD (static scene refresh) ── */
	if (strcmp(cmd, "set-srd") == 0) {
		int enable, level;
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 &&
		    rss_json_get_int(cmd_json, "enable", &enable) == 0 &&
		    rss_json_get_int(cmd_json, "level", &level) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			rss_srd_cfg_t cfg = {.enable = (bool)enable, .level = (uint8_t)level};
			int ret = RSS_HAL_CALL(st->ops, enc_set_srd, st->hal_ctx,
					       st->streams[chn].chn, &cfg);
			return fmt_hal_result(resp, resp_size, ret);
		}
		return rss_ctrl_resp_error(resp, resp_size, "need channel, enable, level");
	}

	if (strcmp(cmd, "get-srd") == 0) {
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			rss_srd_cfg_t cfg = {0};
			int ret = RSS_HAL_CALL(st->ops, enc_get_srd, st->hal_ctx,
					       st->streams[chn].chn, &cfg);
			if (ret == 0) {
				cJSON *r = cJSON_CreateObject();
				cJSON_AddStringToObject(r, "status", "ok");
				cJSON_AddBoolToObject(r, "enable", cfg.enable);
				cJSON_AddNumberToObject(r, "level", (double)cfg.level);
				return rss_ctrl_resp_json(resp, resp_size, r);
			}
			return fmt_hal_result(resp, resp_size, ret);
		}
		return rss_ctrl_resp_error(resp, resp_size, "need channel");
	}

	/* ── Encoder denoise ── */
	if (strcmp(cmd, "set-enc-denoise") == 0) {
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
			return fmt_hal_result(resp, resp_size, ret);
		}
		return rss_ctrl_resp_error(resp, resp_size,
					   "need channel, enable, type, i_qp, p_qp");
	}

	if (strcmp(cmd, "get-enc-denoise") == 0) {
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			rss_enc_denoise_cfg_t cfg = {0};
			int ret = RSS_HAL_CALL(st->ops, enc_get_denoise, st->hal_ctx,
					       st->streams[chn].chn, &cfg);
			if (ret == 0) {
				cJSON *r = cJSON_CreateObject();
				cJSON_AddStringToObject(r, "status", "ok");
				cJSON_AddBoolToObject(r, "enable", cfg.enable);
				cJSON_AddNumberToObject(r, "type", (double)cfg.dn_type);
				cJSON_AddNumberToObject(r, "i_qp", (double)cfg.dn_i_qp);
				cJSON_AddNumberToObject(r, "p_qp", (double)cfg.dn_p_qp);
				return rss_ctrl_resp_json(resp, resp_size, r);
			}
			return fmt_hal_result(resp, resp_size, ret);
		}
		return rss_ctrl_resp_error(resp, resp_size, "need channel");
	}

	/* ── GDR (gradual decoder refresh) ── */
	if (strcmp(cmd, "set-gdr") == 0) {
		int enable, cycle;
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 &&
		    rss_json_get_int(cmd_json, "enable", &enable) == 0 &&
		    rss_json_get_int(cmd_json, "cycle", &cycle) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			rss_enc_gdr_cfg_t cfg = {.enable = (bool)enable, .gdr_cycle = cycle};
			int ret = RSS_HAL_CALL(st->ops, enc_set_gdr, st->hal_ctx,
					       st->streams[chn].chn, &cfg);
			return fmt_hal_result(resp, resp_size, ret);
		}
		return rss_ctrl_resp_error(resp, resp_size, "need channel, enable, cycle");
	}

	if (strcmp(cmd, "get-gdr") == 0) {
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			rss_enc_gdr_cfg_t cfg = {0};
			int ret = RSS_HAL_CALL(st->ops, enc_get_gdr, st->hal_ctx,
					       st->streams[chn].chn, &cfg);
			if (ret == 0) {
				cJSON *r = cJSON_CreateObject();
				cJSON_AddStringToObject(r, "status", "ok");
				cJSON_AddBoolToObject(r, "enable", cfg.enable);
				cJSON_AddNumberToObject(r, "cycle", (double)cfg.gdr_cycle);
				return rss_ctrl_resp_json(resp, resp_size, r);
			}
			return fmt_hal_result(resp, resp_size, ret);
		}
		return rss_ctrl_resp_error(resp, resp_size, "need channel");
	}

	if (strcmp(cmd, "request-gdr") == 0) {
		int frames;
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 &&
		    rss_json_get_int(cmd_json, "value", &frames) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			int ret = RSS_HAL_CALL(st->ops, enc_request_gdr, st->hal_ctx,
					       st->streams[chn].chn, frames);
			return fmt_hal_result(resp, resp_size, ret);
		}
		return rss_ctrl_resp_error(resp, resp_size, "need channel and value");
	}

	/* ── Encoder crop ── */
	if (strcmp(cmd, "set-enc-crop") == 0) {
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
			return fmt_hal_result(resp, resp_size, ret);
		}
		return rss_ctrl_resp_error(resp, resp_size, "need channel, enable, x, y, w, h");
	}

	if (strcmp(cmd, "get-enc-crop") == 0) {
		if (rss_json_get_int(cmd_json, "channel", &chn) == 0 && chn >= 0 &&
		    chn < st->stream_count) {
			rss_enc_crop_cfg_t cfg = {0};
			int ret = RSS_HAL_CALL(st->ops, enc_get_crop, st->hal_ctx,
					       st->streams[chn].chn, &cfg);
			if (ret == 0) {
				cJSON *r = cJSON_CreateObject();
				cJSON_AddStringToObject(r, "status", "ok");
				cJSON_AddBoolToObject(r, "enable", cfg.enable);
				cJSON_AddNumberToObject(r, "x", (double)cfg.x);
				cJSON_AddNumberToObject(r, "y", (double)cfg.y);
				cJSON_AddNumberToObject(r, "w", (double)cfg.w);
				cJSON_AddNumberToObject(r, "h", (double)cfg.h);
				return rss_ctrl_resp_json(resp, resp_size, r);
			}
			return fmt_hal_result(resp, resp_size, ret);
		}
		return rss_ctrl_resp_error(resp, resp_size, "need channel");
	}

	/* ── Encoder capabilities query ── */
	if (strcmp(cmd, "get-enc-caps") == 0) {
		if (!caps) {
			return rss_ctrl_resp_error(resp, resp_size, "caps not available");
		}
		cJSON *r = cJSON_CreateObject();
		cJSON_AddStringToObject(r, "status", "ok");
		cJSON_AddBoolToObject(r, "smartp_gop", caps->has_smartp_gop);
		cJSON_AddBoolToObject(r, "rc_options", caps->has_rc_options);
		cJSON_AddBoolToObject(r, "pskip", caps->has_pskip);
		cJSON_AddBoolToObject(r, "srd", caps->has_srd);
		cJSON_AddBoolToObject(r, "max_pic_size", caps->has_max_pic_size);
		cJSON_AddBoolToObject(r, "super_frame", caps->has_super_frame);
		cJSON_AddBoolToObject(r, "color2grey", caps->has_color2grey);
		cJSON_AddBoolToObject(r, "roi", caps->has_roi);
		cJSON_AddBoolToObject(r, "map_roi", caps->has_map_roi);
		cJSON_AddBoolToObject(r, "qp_bounds_per_frame", caps->has_qp_bounds_per_frame);
		cJSON_AddBoolToObject(r, "qpg_mode", caps->has_qpg_mode);
		cJSON_AddBoolToObject(r, "mbrc", caps->has_mbrc);
		cJSON_AddBoolToObject(r, "enc_denoise", caps->has_enc_denoise);
		cJSON_AddBoolToObject(r, "gdr", caps->has_gdr);
		cJSON_AddBoolToObject(r, "h264_trans", caps->has_h264_trans);
		cJSON_AddBoolToObject(r, "h265_trans", caps->has_h265_trans);
		cJSON_AddBoolToObject(r, "enc_crop", caps->has_enc_crop);
		cJSON_AddBoolToObject(r, "resize_mode", caps->has_resize_mode);
		cJSON_AddBoolToObject(r, "jpeg_ql", caps->has_jpeg_ql);
		cJSON_AddBoolToObject(r, "jpeg_qp", caps->has_jpeg_qp);
		return rss_ctrl_resp_json(resp, resp_size, r);
	}

	(void)caps;
	return 0;
}

/* ── ISP commands ── */

static int handle_isp_cmd(const char *cmd, const char *cmd_json, rvd_state_t *st, char *resp,
			  int resp_size)
{
	int val;
	int sensor_idx = -1;
	rss_json_get_int(cmd_json, "sensor", &sensor_idx);

/* ISP_SET_N: supports --sensor N via _n variant */
#define ISP_SET_N(name, fn)                                                                        \
	if (strcmp(cmd, name) == 0) {                                                              \
		if (rss_json_get_int(cmd_json, "value", &val) == 0) {                              \
			int ret;                                                                   \
			if (sensor_idx >= 0)                                                       \
				ret = RSS_HAL_CALL(st->ops, fn##_n, st->hal_ctx, sensor_idx, val); \
			else                                                                       \
				ret = RSS_HAL_CALL(st->ops, fn, st->hal_ctx, val);                 \
			return fmt_hal_result(resp, resp_size, ret);                               \
		}                                                                                  \
		return rss_ctrl_resp_error(resp, resp_size, "need value");                         \
	}

/* ISP_SET: single-sensor only (no _n variant) */
#define ISP_SET(name, fn)                                                                          \
	if (strcmp(cmd, name) == 0) {                                                              \
		if (rss_json_get_int(cmd_json, "value", &val) == 0) {                              \
			int ret = RSS_HAL_CALL(st->ops, fn, st->hal_ctx, val);                     \
			return fmt_hal_result(resp, resp_size, ret);                               \
		}                                                                                  \
		return rss_ctrl_resp_error(resp, resp_size, "need value");                         \
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

	if (strcmp(cmd, "set-wb") == 0) {
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
		cJSON *r = cJSON_CreateObject();
		cJSON_AddStringToObject(r, "status", ret == 0 ? "ok" : "error");
		cJSON_AddStringToObject(r, "mode", wb_mode_str(wb.mode));
		cJSON_AddNumberToObject(r, "r_gain", (double)wb.r_gain);
		cJSON_AddNumberToObject(r, "b_gain", (double)wb.b_gain);
		return rss_ctrl_resp_json(resp, resp_size, r);
	}

	if (strcmp(cmd, "get-wb") == 0) {
		rss_wb_config_t wb = {0};
		RSS_HAL_CALL(st->ops, isp_get_wb, st->hal_ctx, &wb);
		cJSON *r = cJSON_CreateObject();
		cJSON_AddStringToObject(r, "status", "ok");
		cJSON_AddStringToObject(r, "mode", wb_mode_str(wb.mode));
		cJSON_AddNumberToObject(r, "r_gain", (double)wb.r_gain);
		cJSON_AddNumberToObject(r, "g_gain", (double)wb.g_gain);
		cJSON_AddNumberToObject(r, "b_gain", (double)wb.b_gain);
		return rss_ctrl_resp_json(resp, resp_size, r);
	}

	if (strcmp(cmd, "get-isp") == 0) {
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
		cJSON *r = cJSON_CreateObject();
		cJSON_AddStringToObject(r, "status", "ok");
		cJSON_AddNumberToObject(r, "brightness", (double)bri);
		cJSON_AddNumberToObject(r, "contrast", (double)con);
		cJSON_AddNumberToObject(r, "saturation", (double)sat);
		cJSON_AddNumberToObject(r, "sharpness", (double)shp);
		cJSON_AddNumberToObject(r, "hue", (double)hue);
		cJSON_AddNumberToObject(r, "sinter", (double)sin);
		cJSON_AddNumberToObject(r, "temper", (double)tem);
		cJSON_AddNumberToObject(r, "hflip", (double)hf);
		cJSON_AddNumberToObject(r, "vflip", (double)vf);
		cJSON_AddNumberToObject(r, "ae_comp", (double)ae);
		cJSON_AddNumberToObject(r, "max_again", (double)again);
		cJSON_AddNumberToObject(r, "max_dgain", (double)dgain);
		cJSON_AddStringToObject(r, "wb_mode", wb.mode == RSS_WB_MANUAL ? "manual" : "auto");
		cJSON_AddNumberToObject(r, "wb_r", (double)wb.r_gain);
		cJSON_AddNumberToObject(r, "wb_g", (double)wb.g_gain);
		cJSON_AddNumberToObject(r, "wb_b", (double)wb.b_gain);
		return rss_ctrl_resp_json(resp, resp_size, r);
	}

	if (strcmp(cmd, "get-exposure") == 0) {
		rss_exposure_t exp = {0};
		RSS_HAL_CALL(st->ops, isp_get_exposure, st->hal_ctx, &exp);
		cJSON *r = cJSON_CreateObject();
		cJSON_AddNumberToObject(r, "total_gain", (double)exp.total_gain);
		cJSON_AddNumberToObject(r, "exposure_us", (double)exp.exposure_time);
		cJSON_AddNumberToObject(r, "ae_luma", (double)exp.ae_luma);
		cJSON_AddNumberToObject(r, "ev", (double)exp.ev);
		cJSON_AddNumberToObject(r, "wb_rgain", (double)exp.wb_rgain);
		cJSON_AddNumberToObject(r, "wb_bgain", (double)exp.wb_bgain);
		return rss_ctrl_resp_json(resp, resp_size, r);
	}

	if (strcmp(cmd, "set-running-mode") == 0) {
		char mode_str[8];
		if (rss_json_get_str(cmd_json, "value", mode_str, sizeof(mode_str)) == 0) {
			int mode = (strcmp(mode_str, "night") == 0) ? 1 : 0;
			RSS_HAL_CALL(st->ops, isp_set_running_mode, st->hal_ctx, mode);
			cJSON *r = cJSON_CreateObject();
			cJSON_AddStringToObject(r, "status", "ok");
			cJSON_AddStringToObject(r, "mode", mode ? "night" : "day");
			return rss_ctrl_resp_json(resp, resp_size, r);
		}
		return rss_ctrl_resp_error(resp, resp_size, "need value");
	}

	return 0;
}

/* ── IVS commands ── */

static int handle_ivs_cmd(const char *cmd, const char *cmd_json, rvd_state_t *st, char *resp,
			  int resp_size)
{
	if (strcmp(cmd, "ivs-status") == 0) {
		bool motion = atomic_load(&st->ivs_motion);
		int64_t ts = atomic_load(&st->ivs_motion_ts);
		int persons = st->ivs_persondet ? atomic_load(&st->ivs_person_count) : -1;
		cJSON *r = cJSON_CreateObject();
		cJSON_AddStringToObject(r, "status", "ok");
		cJSON_AddBoolToObject(r, "active", st->ivs_active);
		cJSON_AddBoolToObject(r, "motion", motion);
		cJSON_AddBoolToObject(r, "persondet", st->ivs_persondet);
		cJSON_AddNumberToObject(r, "persons", (double)persons);
		cJSON_AddNumberToObject(r, "timestamp", (double)ts);
		return rss_ctrl_resp_json(resp, resp_size, r);
	}

	if (strcmp(cmd, "ivs-detections") == 0) {
		if (!st->ivs_active) {
			return rss_ctrl_resp_error(resp, resp_size, "ivs not active");
		}
		cJSON *r = cJSON_CreateObject();
		cJSON_AddStringToObject(r, "status", "ok");
		pthread_mutex_lock(&st->ivs_det_lock);
		int count = st->ivs_detections.count;
		cJSON_AddNumberToObject(r, "count", (double)count);
		cJSON *arr = cJSON_AddArrayToObject(r, "detections");
		for (int i = 0; i < count; i++) {
			rss_ivs_detection_t *d = &st->ivs_detections.detections[i];
			cJSON *item = cJSON_CreateObject();
			cJSON_AddNumberToObject(item, "x0", (double)d->box.p0_x);
			cJSON_AddNumberToObject(item, "y0", (double)d->box.p0_y);
			cJSON_AddNumberToObject(item, "x1", (double)d->box.p1_x);
			cJSON_AddNumberToObject(item, "y1", (double)d->box.p1_y);
			cJSON_AddNumberToObject(item, "confidence", (double)d->confidence);
			cJSON_AddNumberToObject(item, "class", (double)d->class_id);
			cJSON_AddItemToArray(arr, item);
		}
		pthread_mutex_unlock(&st->ivs_det_lock);
		return rss_ctrl_resp_json(resp, resp_size, r);
	}

	if (strcmp(cmd, "ivs-set-sensitivity") == 0) {
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
			cJSON *r = cJSON_CreateObject();
			cJSON_AddStringToObject(r, "status", "ok");
			cJSON_AddNumberToObject(r, "sensitivity", (double)sens);
			return rss_ctrl_resp_json(resp, resp_size, r);
		}
		return rss_ctrl_resp_error(resp, resp_size, "invalid or ivs not active");
	}

	if (strcmp(cmd, "ivs-set-skip-frames") == 0) {
		int val = -1;
		if (rss_json_get_int(cmd_json, "value", &val) == 0 && st->ivs_active && val >= 0) {
			rss_ivs_move_param_t mp;
			memset(&mp, 0, sizeof(mp));
			if (RSS_HAL_CALL(st->ops, ivs_get_param, st->hal_ctx, st->ivs_chn, &mp) ==
			    0) {
				mp.skip_frame_count = val;
				RSS_HAL_CALL(st->ops, ivs_set_param, st->hal_ctx, st->ivs_chn, &mp);
			}
			cJSON *r = cJSON_CreateObject();
			cJSON_AddStringToObject(r, "status", "ok");
			cJSON_AddNumberToObject(r, "skip_frames", (double)val);
			return rss_ctrl_resp_json(resp, resp_size, r);
		}
		return rss_ctrl_resp_error(resp, resp_size, "invalid or ivs not active");
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
		rvd_ivs_pause(st); /* StopRecvPic — must be before active=false */
		atomic_store(&st->ivs_active, false); /* signal thread exit */
		if (st->ivs_tid) {
			pthread_join(st->ivs_tid, NULL);
			st->ivs_tid = 0;
		}
	}
	if (jpeg >= 0)
		rvd_stream_stop(st, jpeg);
	rvd_stream_stop(st, chn);

	/* Deinit: JPEG → video. IVS channel stays alive (SDK limitation). */
	if (jpeg >= 0)
		rvd_stream_deinit(st, jpeg);
	rvd_stream_deinit(st, chn);

	if (has_ivs)
		atomic_store(&st->ivs_active, true); /* re-enable for bind chain */

	int ret = rvd_stream_init(st, chn);
	if (ret != RSS_OK) {
		RSS_ERROR("stream-restart: init stream %d failed: %d", chn, ret);
		if (has_ivs)
			atomic_store(&st->ivs_active, false);
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

	/* Start: video → JPEG, then IVS (SDK requires FS streaming before IVS channel) */
	rvd_stream_start(st, chn);
	if (jpeg >= 0 && st->streams[jpeg].ring)
		rvd_stream_start(st, jpeg);

	/* IVS resume + thread relaunch */
	if (has_ivs) {
		rvd_ivs_resume(st);
		pthread_attr_t ivs_attr;
		pthread_attr_init(&ivs_attr);
		pthread_attr_setstacksize(&ivs_attr, 64 * 1024);
		if (pthread_create(&st->ivs_tid, &ivs_attr, rvd_ivs_thread, st) != 0)
			RSS_ERROR("ivs thread create failed");
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

static int handle_pipeline_cmd(const char *cmd, const char *cmd_json, rvd_state_t *st, char *resp,
			       int resp_size)
{
	int chn;

	if (strcmp(cmd, "stream-restart") == 0) {
		if (validate_video_channel(st, cmd_json, &chn, resp, resp_size) < 0)
			return (int)strlen(resp); /* validate already wrote error */
		if (do_stream_restart(st, chn, resp, resp_size) < 0)
			return (int)strlen(resp);
		return rss_ctrl_resp_ok(resp, resp_size);
	}

	if (strcmp(cmd, "stream-stop") == 0) {
		if (validate_video_channel(st, cmd_json, &chn, resp, resp_size) < 0)
			return (int)strlen(resp);
		if (!atomic_load(&st->stream_active[chn]))
			return rss_ctrl_resp_ok(resp, resp_size);

		int jpeg = find_jpeg_for_video_ctrl(st, chn);
		bool has_ivs = (st->streams[chn].fs_chn == 1 && st->ivs_active);

		RSS_INFO("stream-stop: channel %d (jpeg=%d ivs=%d)", chn, jpeg, has_ivs);

		if (has_ivs) {
			rvd_ivs_pause(st);
			atomic_store(&st->ivs_active, false);
			if (st->ivs_tid) {
				pthread_join(st->ivs_tid, NULL);
				st->ivs_tid = 0;
			}
		}
		if (jpeg >= 0 && atomic_load(&st->stream_active[jpeg]))
			rvd_stream_stop(st, jpeg);
		rvd_stream_stop(st, chn);

		RSS_INFO("stream-stop: channel %d stopped", chn);
		return rss_ctrl_resp_ok(resp, resp_size);
	}

	if (strcmp(cmd, "stream-start") == 0) {
		if (validate_video_channel(st, cmd_json, &chn, resp, resp_size) < 0)
			return (int)strlen(resp);
		if (atomic_load(&st->stream_active[chn]))
			return rss_ctrl_resp_ok(resp, resp_size);
		if (!st->streams[chn].ring)
			return rss_ctrl_resp_error(resp, resp_size, "stream not initialized");

		int jpeg = find_jpeg_for_video_ctrl(st, chn);
		bool has_ivs = (st->streams[chn].fs_chn == 1 && st->ivs_enabled && !st->ivs_active);

		RSS_INFO("stream-start: channel %d (jpeg=%d ivs=%d)", chn, jpeg, has_ivs);

		rvd_stream_start(st, chn);
		if (jpeg >= 0 && st->streams[jpeg].ring && !atomic_load(&st->stream_active[jpeg]))
			rvd_stream_start(st, jpeg);

		if (has_ivs) {
			atomic_store(&st->ivs_active, true);
			rvd_ivs_resume(st);
			pthread_attr_t ivs_attr;
			pthread_attr_init(&ivs_attr);
			pthread_attr_setstacksize(&ivs_attr, 64 * 1024);
			if (pthread_create(&st->ivs_tid, &ivs_attr, rvd_ivs_thread, st) != 0) {
				RSS_ERROR("ivs thread create failed");
				atomic_store(&st->ivs_active, false);
			}
			pthread_attr_destroy(&ivs_attr);
		}

		RSS_INFO("stream-start: channel %d started", chn);
		return rss_ctrl_resp_ok(resp, resp_size);
	}

	if (strcmp(cmd, "set-codec") == 0) {
		char val[8];
		if (validate_video_channel(st, cmd_json, &chn, resp, resp_size) < 0)
			return (int)strlen(resp);
		if (rss_json_get_str(cmd_json, "value", val, sizeof(val)) != 0) {
			return rss_ctrl_resp_error(resp, resp_size, "need value (h264|h265)");
		}

		rss_codec_t codec;
		if (strcasecmp(val, "h265") == 0 || strcasecmp(val, "hevc") == 0)
			codec = RSS_CODEC_H265;
		else if (strcasecmp(val, "h264") == 0 || strcasecmp(val, "avc") == 0)
			codec = RSS_CODEC_H264;
		else {
			return rss_ctrl_resp_error(resp, resp_size, "unknown codec");
		}

		/* Check H.265 capability */
		const rss_hal_caps_t *caps =
			st->ops->get_caps ? st->ops->get_caps(st->hal_ctx) : NULL;
		if (codec == RSS_CODEC_H265 && caps && !caps->has_h265) {
			return rss_ctrl_resp_error(resp, resp_size, "h265 not supported");
		}

		if (st->streams[chn].enc_cfg.codec == codec) {
			return rss_ctrl_resp_ok(resp, resp_size); /* already set */
		}

		/* Save old codec, update, restart, restore on failure */
		rss_codec_t old_codec = st->streams[chn].enc_cfg.codec;
		st->streams[chn].enc_cfg.codec = codec;
		RSS_INFO("set-codec: channel %d → %s", chn, val);

		if (do_stream_restart(st, chn, resp, resp_size) < 0) {
			st->streams[chn].enc_cfg.codec = old_codec;
			return (int)strlen(resp);
		}
		rss_config_set_str(st->cfg, st->streams[chn].cfg_sect, "codec", val);
		return rss_ctrl_resp_ok(resp, resp_size);
	}

	if (strcmp(cmd, "set-resolution") == 0) {
		int w, h;
		if (validate_video_channel(st, cmd_json, &chn, resp, resp_size) < 0)
			return (int)strlen(resp);
		if (rss_json_get_int(cmd_json, "width", &w) != 0 ||
		    rss_json_get_int(cmd_json, "height", &h) != 0 || w < 32 || h < 32 || w > 4096 ||
		    h > 4096) {
			return rss_ctrl_resp_error(resp, resp_size,
						   "need width and height (32-4096)");
		}

		/* Even dimensions required by encoder */
		w = (w + 1) & ~1;
		h = (h + 1) & ~1;

		if ((int)st->streams[chn].enc_cfg.width == w &&
		    (int)st->streams[chn].enc_cfg.height == h) {
			return rss_ctrl_resp_ok(resp, resp_size);
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
			rvd_ivs_pause(st);
			atomic_store(&st->ivs_active, false);
			if (st->ivs_tid) {
				pthread_join(st->ivs_tid, NULL);
				st->ivs_tid = 0;
			}
		}
		if (jpeg >= 0)
			rvd_stream_stop(st, jpeg);
		rvd_stream_stop(st, chn);

		/* Deinit (IVS channel stays alive — SDK limitation) */
		if (jpeg >= 0)
			rvd_stream_deinit(st, jpeg);
		rvd_stream_deinit(st, chn);

		/* Reconfigure FS scaler (channel disabled, not destroyed) */
		RSS_HAL_CALL(st->ops, fs_set_channel_attr, st->hal_ctx, st->streams[chn].fs_chn,
			     &st->streams[chn].fs_cfg);

		/* Reinit */
		if (has_ivs)
			atomic_store(&st->ivs_active, true);

		int ret = rvd_stream_init(st, chn);
		if (ret != RSS_OK) {
			RSS_ERROR("set-resolution: init failed: %d, restoring old config", ret);
			st->streams[chn].enc_cfg.width = old_enc_w;
			st->streams[chn].enc_cfg.height = old_enc_h;
			st->streams[chn].fs_cfg.width = old_fs_w;
			st->streams[chn].fs_cfg.height = old_fs_h;
			st->streams[chn].fs_cfg.scaler.out_width = old_sc_w;
			st->streams[chn].fs_cfg.scaler.out_height = old_sc_h;
			/* Restore FS scaler and attempt re-init with old config */
			RSS_HAL_CALL(st->ops, fs_set_channel_attr, st->hal_ctx,
				     st->streams[chn].fs_chn, &st->streams[chn].fs_cfg);
			if (rvd_stream_init(st, chn) == RSS_OK)
				rvd_stream_start(st, chn);
			if (has_ivs)
				atomic_store(&st->ivs_active, false);
			return rss_ctrl_resp_error(resp, resp_size, "init failed");
		}
		if (jpeg >= 0) {
			st->streams[jpeg].enc_cfg.width = w;
			st->streams[jpeg].enc_cfg.height = h;
			ret = rvd_stream_init(st, jpeg);
			if (ret != RSS_OK)
				RSS_WARN("set-resolution: jpeg init failed: %d", ret);
		}

		/* Start streams first, then IVS (SDK requires FS streaming) */
		rvd_stream_start(st, chn);
		if (jpeg >= 0 && st->streams[jpeg].ring)
			rvd_stream_start(st, jpeg);
		if (has_ivs) {
			rvd_ivs_resume(st);
			pthread_attr_t ivs_attr;
			pthread_attr_init(&ivs_attr);
			pthread_attr_setstacksize(&ivs_attr, 64 * 1024);
			if (pthread_create(&st->ivs_tid, &ivs_attr, rvd_ivs_thread, st) != 0)
				RSS_ERROR("ivs thread create failed");
			pthread_attr_destroy(&ivs_attr);
		}

		/* Persist config only after successful restart */
		rss_config_set_int(st->cfg, st->streams[chn].cfg_sect, "width", w);
		rss_config_set_int(st->cfg, st->streams[chn].cfg_sect, "height", h);
		RSS_INFO("set-resolution: channel %d → %dx%d complete", chn, w, h);
		return rss_ctrl_resp_ok(resp, resp_size);
	}

	if (strcmp(cmd, "osd-show") == 0) {
		int stream = -1, show = -1;
		char region[RVD_OSD_NAME_LEN] = "";
		rss_json_get_int(cmd_json, "stream", &stream);
		rss_json_get_int(cmd_json, "show", &show);
		rss_json_get_str(cmd_json, "region", region, sizeof(region));

		if (stream < 0 || stream >= st->stream_count || show < 0 || !region[0])
			return rss_ctrl_resp_error(resp, resp_size, "need stream, region, show");

		rvd_osd_region_t *reg = rvd_osd_find_region(st, stream, region);
		if (!reg || reg->hal_handle < 0)
			return rss_ctrl_resp_error(resp, resp_size, "region not active");

		pthread_mutex_lock(&st->osd_lock);
		if (st->use_isp_osd && st->streams[stream].fs_chn % 3 == 0) {
			int sensor = st->streams[stream].sensor_idx;
			RSS_HAL_CALL(st->ops, isp_osd_show_region, st->hal_ctx, sensor,
				     reg->hal_handle, show);
		} else {
			int grp = st->streams[stream].chn;
			RSS_HAL_CALL(st->ops, osd_show_region, st->hal_ctx, reg->hal_handle, grp,
				     show, reg->layer + 1);
		}
		reg->shown = !!show;
		pthread_mutex_unlock(&st->osd_lock);

		RSS_DEBUG("osd-show: stream %d %s %s", stream, region, show ? "on" : "off");
		return rss_ctrl_resp_ok(resp, resp_size);
	}

	if (strcmp(cmd, "osd-position") == 0) {
		int stream = -1;
		char region[RVD_OSD_NAME_LEN] = "", pos[32] = "";
		rss_json_get_int(cmd_json, "stream", &stream);
		rss_json_get_str(cmd_json, "region", region, sizeof(region));
		rss_json_get_str(cmd_json, "pos", pos, sizeof(pos));

		if (stream < 0 || stream >= st->stream_count || !region[0] || !pos[0])
			return rss_ctrl_resp_error(resp, resp_size, "need stream, region, pos");

		/* Always save position to config — even if region doesn't
		 * exist yet. scan_new_shm reads it when creating the region. */
		char pos_key[64];
		snprintf(pos_key, sizeof(pos_key), "stream%d_%s_pos", stream, region);
		rss_config_set_str(st->cfg, "osd", pos_key, pos);

		rvd_osd_region_t *reg = rvd_osd_find_region(st, stream, region);
		if (!reg || reg->hal_handle < 0) {
			RSS_DEBUG("osd-position: stream %d %s -> %s (saved, region pending)",
				  stream, region, pos);
			return rss_ctrl_resp_ok(resp, resp_size);
		}

		int stream_w = st->streams[stream].enc_cfg.width;
		int stream_h = st->streams[stream].enc_cfg.height;
		int x, y;
		rvd_osd_calc_position(stream_w, stream_h, (int)reg->width, (int)reg->height, pos,
				      &x, &y);

		pthread_mutex_lock(&st->osd_lock);
		rss_osd_region_t attr = {
			.type = RSS_OSD_PIC,
			.x = x,
			.y = y,
			.width = (int)reg->width,
			.height = (int)reg->height,
			.bitmap_data = reg->local_buf,
			.bitmap_fmt = RSS_PIXFMT_BGRA,
		};
		if (st->use_isp_osd && st->streams[stream].fs_chn % 3 == 0) {
			int sensor = st->streams[stream].sensor_idx;
			int chx = st->streams[stream].fs_chn % 3;
			RSS_HAL_CALL(st->ops, isp_osd_set_region_attr, st->hal_ctx, sensor,
				     reg->hal_handle, chx, &attr);
		} else {
			RSS_HAL_CALL(st->ops, osd_set_region_attr, st->hal_ctx, reg->hal_handle,
				     &attr);
		}
		pthread_mutex_unlock(&st->osd_lock);

		RSS_DEBUG("osd-position: stream %d %s -> %s (%d,%d)", stream, reg->name, pos, x, y);
		return rss_ctrl_resp_ok(resp, resp_size);
	}

	if (strcmp(cmd, "osd-restart") == 0) {
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
			rvd_ivs_pause(st);
			atomic_store(&st->ivs_active, false);
			if (st->ivs_tid) {
				pthread_join(st->ivs_tid, NULL);
				st->ivs_tid = 0;
			}
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
		/* IVS group stays alive (SDK limitation — can't recreate) */

		/* Store ROD's requested pool size so rvd_stream_init uses it */
		if (pool_kb > 0) {
			uint32_t new_pool = (uint32_t)pool_kb * 1024;
			st->osd_pool_override = new_pool;
			RSS_INFO("osd-restart: pool override %u KB", pool_kb);
			int ret = RSS_HAL_CALL(st->ops, osd_set_pool_size, st->hal_ctx, new_pool);
			RSS_INFO("osd-restart: osd_set_pool_size returned %d", ret);
		}

		if (has_ivs)
			atomic_store(&st->ivs_active, true);

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

		/* Start all: video first, then JPEG (skip failed inits) */
		for (int j = 0; j < video_count; j++) {
			if (init_ok[video_indices[j]])
				rvd_stream_start(st, video_indices[j]);
		}
		for (int j = 0; j < jpeg_count_local; j++) {
			if (init_ok[jpeg_indices[j]])
				rvd_stream_start(st, jpeg_indices[j]);
		}

		/* IVS start + thread relaunch (after streams — SDK requires FS streaming) */
		if (has_ivs) {
			rvd_ivs_resume(st);
			pthread_attr_t ivs_attr;
			pthread_attr_init(&ivs_attr);
			pthread_attr_setstacksize(&ivs_attr, 64 * 1024);
			if (pthread_create(&st->ivs_tid, &ivs_attr, rvd_ivs_thread, st) != 0)
				RSS_ERROR("ivs thread create failed");
			pthread_attr_destroy(&ivs_attr);
		}

		RSS_INFO("osd-restart: complete");
		return rss_ctrl_resp_ok(resp, resp_size);
	}

	return 0;
}

/* ── Config and status commands ── */

static int handle_config_cmd(const char *cmd, const char *cmd_json, rvd_state_t *st, char *resp,
			     int resp_size)
{
	if (strcmp(cmd, "config-show") == 0) {
		cJSON *r = cJSON_CreateObject();
		cJSON_AddStringToObject(r, "status", "ok");
		cJSON *cfg = cJSON_AddObjectToObject(r, "config");
		cJSON *arr = cJSON_AddArrayToObject(cfg, "streams");
		for (int i = 0; i < st->stream_count; i++) {
			rvd_stream_t *s = &st->streams[i];
			uint32_t avg_br = 0;
			RSS_HAL_CALL(st->ops, enc_get_avg_bitrate, st->hal_ctx, s->chn, &avg_br);
			cJSON *item = cJSON_CreateObject();
			cJSON_AddNumberToObject(item, "chn", (double)s->chn);
			cJSON_AddNumberToObject(item, "w", (double)s->enc_cfg.width);
			cJSON_AddNumberToObject(item, "h", (double)s->enc_cfg.height);
			cJSON_AddNumberToObject(item, "codec", (double)s->enc_cfg.codec);
			cJSON_AddNumberToObject(item, "bitrate", (double)s->enc_cfg.bitrate);
			cJSON_AddNumberToObject(item, "avg_bitrate", (double)avg_br);
			cJSON_AddNumberToObject(item, "gop", (double)s->enc_cfg.gop_length);
			cJSON_AddNumberToObject(item, "fps", (double)s->enc_cfg.fps_num);
			cJSON_AddNumberToObject(item, "min_qp", (double)s->enc_cfg.min_qp);
			cJSON_AddNumberToObject(item, "max_qp", (double)s->enc_cfg.max_qp);
			cJSON_AddNumberToObject(item, "rc_mode", (double)s->enc_cfg.rc_mode);
			cJSON_AddNumberToObject(item, "profile", (double)s->enc_cfg.profile);
			cJSON_AddItemToArray(arr, item);
		}
		cJSON_AddStringToObject(cfg, "config_path", st->config_path);
		return rss_ctrl_resp_json(resp, resp_size, r);
	}

	if (strcmp(cmd, "privacy") == 0) {
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

		cJSON *r = cJSON_CreateObject();
		cJSON_AddStringToObject(r, "status", "ok");
		cJSON *arr = cJSON_AddArrayToObject(r, "privacy");
		for (int i = 0; i < st->stream_count; i++) {
			if (st->streams[i].is_jpeg)
				continue;
			cJSON_AddItemToArray(arr,
					     cJSON_CreateString(st->privacy[i] ? "on" : "off"));
		}
		return rss_ctrl_resp_json(resp, resp_size, r);
	}

	if (strcmp(cmd, "status") == 0) {
		cJSON *r = cJSON_CreateObject();
		cJSON_AddStringToObject(r, "status", "ok");
		cJSON *arr = cJSON_AddArrayToObject(r, "streams");
		for (int i = 0; i < st->stream_count; i++) {
			uint32_t avg_br = 0;
			RSS_HAL_CALL(st->ops, enc_get_avg_bitrate, st->hal_ctx, st->streams[i].chn,
				     &avg_br);
			cJSON *item = cJSON_CreateObject();
			cJSON_AddNumberToObject(item, "chn", (double)st->streams[i].chn);
			cJSON_AddNumberToObject(item, "w", (double)st->streams[i].enc_cfg.width);
			cJSON_AddNumberToObject(item, "h", (double)st->streams[i].enc_cfg.height);
			cJSON_AddNumberToObject(item, "codec",
						(double)st->streams[i].enc_cfg.codec);
			cJSON_AddNumberToObject(item, "bitrate",
						(double)st->streams[i].enc_cfg.bitrate);
			cJSON_AddNumberToObject(item, "avg_bitrate", (double)avg_br);
			cJSON_AddNumberToObject(item, "gop",
						(double)st->streams[i].enc_cfg.gop_length);
			cJSON_AddNumberToObject(item, "fps",
						(double)st->streams[i].enc_cfg.fps_num);
			cJSON_AddItemToArray(arr, item);
		}
		return rss_ctrl_resp_json(resp, resp_size, r);
	}

	return 0;
}

/* ── Main dispatch ── */

int rvd_ctrl_handler(const char *cmd_json, char *resp_buf, int resp_buf_size, void *userdata)
{
	rvd_state_t *st = userdata;
	int len;

	int rc =
		rss_ctrl_handle_common(cmd_json, resp_buf, resp_buf_size, st->cfg, st->config_path);
	if (rc >= 0)
		return rc;

	char cmd[64];
	if (rss_json_get_str(cmd_json, "cmd", cmd, sizeof(cmd)) != 0)
		return rss_ctrl_resp_error(resp_buf, resp_buf_size, "missing cmd");

	if ((len = handle_encoder_cmd(cmd, cmd_json, st, resp_buf, resp_buf_size)) > 0)
		return len;
	if ((len = handle_encoder_advanced_cmd(cmd, cmd_json, st, resp_buf, resp_buf_size)) > 0)
		return len;
	if ((len = handle_isp_cmd(cmd, cmd_json, st, resp_buf, resp_buf_size)) > 0)
		return len;
	if ((len = handle_ivs_cmd(cmd, cmd_json, st, resp_buf, resp_buf_size)) > 0)
		return len;
	if ((len = handle_pipeline_cmd(cmd, cmd_json, st, resp_buf, resp_buf_size)) > 0)
		return len;
	if ((len = handle_config_cmd(cmd, cmd_json, st, resp_buf, resp_buf_size)) > 0)
		return len;

	return rss_ctrl_resp_error(resp_buf, resp_buf_size, "unknown command");
}

/*
 * rod_ctrl.c -- Control socket command handlers
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rod.h"

static uint32_t calc_osd_pool_kb(rod_state_t *st)
{
	uint32_t total = 0;
	for (int i = 0; i < st->elem_count; i++) {
		rod_element_t *e = &st->elements[i];
		if (!e->active)
			continue;
		for (int s = 0; s < st->stream_count; s++) {
			if (e->streams[s].shm)
				total += e->streams[s].width * e->streams[s].height * 4;
		}
	}
	total = total * 5 / 4;
	uint32_t kb = (total + 1023) / 1024;
	if (kb < 448)
		kb = 448;
	return kb;
}

static void notify_rvd_osd_restart(rod_state_t *st)
{
	uint32_t pool_kb = calc_osd_pool_kb(st);
	cJSON *j = cJSON_CreateObject();
	if (j) {
		char fwd[96];
		cJSON_AddStringToObject(j, "cmd", "osd-restart");
		cJSON_AddNumberToObject(j, "pool_kb", pool_kb);
		cJSON_PrintPreallocated(j, fwd, sizeof(fwd), 0);
		cJSON_Delete(j);
		char rvd_resp[256];
		rss_ctrl_send_command(ROD_RVD_SOCK, fwd, rvd_resp, sizeof(rvd_resp), 5000);
		RSS_INFO("osd-restart: pool_kb=%u", pool_kb);
	}
}

static int handle_color_change(rod_state_t *st, const char *cmd_json, char *resp, int resp_size,
			       const char *key, uint32_t *target)
{
	char val[16];
	if (rss_json_get_str(cmd_json, "value", val, sizeof(val)) != 0)
		return rss_ctrl_resp_error(resp, resp_size, "missing value");

	*target = parse_color(val);
	rss_config_set_str(st->cfg, "osd", key, val);
	mark_all_dirty(st);
	return rss_ctrl_resp_ok(resp, resp_size);
}

static int handle_set_position(rod_state_t *st, const char *cmd_json, char *resp, int resp_size)
{
	char element[32] = "", pos[32] = "";
	rss_json_get_str(cmd_json, "element", element, sizeof(element));
	rss_json_get_str(cmd_json, "pos", pos, sizeof(pos));

	if (!element[0] || !pos[0])
		return rss_ctrl_resp_error(resp, resp_size, "need element and pos");

	int target_stream = -1;
	rss_json_get_int(cmd_json, "stream", &target_stream);
	for (int s = 0; s < st->stream_count; s++) {
		if (target_stream >= 0 && s != target_stream)
			continue;
		char fwd[128];
		{
			cJSON *j = cJSON_CreateObject();
			if (!j)
				continue;
			cJSON_AddStringToObject(j, "cmd", "osd-position");
			cJSON_AddNumberToObject(j, "stream", s);
			cJSON_AddStringToObject(j, "region", element);
			cJSON_AddStringToObject(j, "pos", pos);
			cJSON_PrintPreallocated(j, fwd, sizeof(fwd), 0);
			cJSON_Delete(j);
		}
		char rvd_resp[256];
		rss_ctrl_send_command(ROD_RVD_SOCK, fwd, rvd_resp, sizeof(rvd_resp), 1000);
	}

	RSS_INFO("set-position: %s -> %s", element, pos);
	return rss_ctrl_resp_ok(resp, resp_size);
}

static int handle_font_size_change(rod_state_t *st, const char *cmd_json, char *resp, int resp_size)
{
	int val;
	if (rss_json_get_int(cmd_json, "value", &val) != 0 || val < 10 || val > 72)
		return rss_ctrl_resp_error(resp, resp_size, "need value 10-72");

	RSS_INFO("set-font-size: %d -> %d", st->settings.font_size, val);
	st->settings.font_size = val;
	rss_config_set_int(st->cfg, "osd", "font_size", val);

	for (int i = 0; i < st->elem_count; i++) {
		rod_element_t *e = &st->elements[i];
		if (!e->active || e->type != ROD_ELEM_TEXT)
			continue;

		int new_size = e->font_size > 0 ? e->font_size : val;

		for (int s = 0; s < st->stream_count; s++) {
			int fs = new_size;
			if (s > 0 && st->stream_h[0] > 0) {
				fs = fs * st->stream_h[s] / st->stream_h[0];
				if (fs < 12)
					fs = 12;
			}

			release_font(st, s, e->streams[s].font_idx);
			int fi = rod_alloc_font(st, s, fs);
			if (fi < 0) {
				RSS_ERROR("font alloc failed s%d elem %s", s, e->name);
				continue;
			}
			e->streams[s].font_idx = fi;

			if (e->streams[s].shm) {
				rss_osd_destroy(e->streams[s].shm);
				e->streams[s].shm = NULL;
			}
			create_elem_shm(st, e, s);
		}
	}

	notify_rvd_osd_restart(st);

	return rss_ctrl_resp_ok(resp, resp_size);
}

static int handle_elements_list(rod_state_t *st, char *resp, int resp_size)
{
	static const char *type_names[] = {"text", "image", "overlay", "receipt"};

	cJSON *r = cJSON_CreateObject();
	if (!r)
		return rss_ctrl_resp_error(resp, resp_size, "alloc");
	cJSON_AddStringToObject(r, "status", "ok");
	cJSON *arr = cJSON_AddArrayToObject(r, "elements");
	for (int i = 0; i < st->elem_count; i++) {
		rod_element_t *e = &st->elements[i];
		if (!e->active)
			continue;
		cJSON *obj = cJSON_CreateObject();
		if (!obj)
			continue;
		cJSON_AddStringToObject(obj, "name", e->name);
		cJSON_AddStringToObject(obj, "type", type_names[e->type]);
		cJSON_AddBoolToObject(obj, "visible", e->visible);
		cJSON_AddStringToObject(obj, "position", e->position);
		if (e->type == ROD_ELEM_TEXT)
			cJSON_AddStringToObject(obj, "template", e->tmpl);
		int fs = e->font_size > 0 ? e->font_size : st->settings.font_size;
		cJSON_AddNumberToObject(obj, "font_size", fs);
		cJSON_AddItemToArray(arr, obj);
	}
	return rss_ctrl_resp_json(resp, resp_size, r);
}

static int handle_add_element(rod_state_t *st, const char *cmd_json, char *resp, int resp_size)
{
	char name[ROD_ELEM_NAME_LEN] = "";
	char type_str[16] = "text";
	char tmpl[ROD_TMPL_LEN] = "";
	char position[32] = "top_left";
	int font_size = 0, max_chars = 20, align = 0;

	rss_json_get_str(cmd_json, "name", name, sizeof(name));
	rss_json_get_str(cmd_json, "type", type_str, sizeof(type_str));
	rss_json_get_str(cmd_json, "template", tmpl, sizeof(tmpl));
	rss_json_get_str(cmd_json, "position", position, sizeof(position));
	rss_json_get_int(cmd_json, "font_size", &font_size);
	rss_json_get_int(cmd_json, "max_chars", &max_chars);
	rss_json_get_int(cmd_json, "align", &align);

	if (!name[0])
		return rss_ctrl_resp_error(resp, resp_size, "need name");

	sanitize_text(tmpl);

	rod_elem_type_t type = ROD_ELEM_TEXT;
	if (strcmp(type_str, "image") == 0)
		type = ROD_ELEM_IMAGE;
	else if (strcmp(type_str, "overlay") == 0)
		type = ROD_ELEM_OVERLAY;
	else if (strcmp(type_str, "receipt") == 0)
		type = ROD_ELEM_RECEIPT;

	int ret = rod_add_element(st, name, type, tmpl, position, align, font_size, max_chars,
				  ROD_UPDATE_TICK);
	if (ret < 0)
		return rss_ctrl_resp_error(resp, resp_size, "add failed (exists or full)");

	rod_element_t *e = rod_find_element(st, name);
	if (!e)
		return rss_ctrl_resp_error(resp, resp_size, "internal error");

	if (e->type == ROD_ELEM_TEXT || e->type == ROD_ELEM_RECEIPT) {
		if (e->type == ROD_ELEM_RECEIPT) {
			e->receipt.max_lines = max_chars > 0 ? max_chars : 10;
			if (e->receipt.max_lines > ROD_RECEIPT_MAX_LINES)
				e->receipt.max_lines = ROD_RECEIPT_MAX_LINES;
			e->receipt.max_line_len = ROD_RECEIPT_MAX_LINE;
			e->receipt.bg_color = 0x80000000;
			e->receipt.input_fd = -1;

			int ml = 0, mll = 0;
			rss_json_get_int(cmd_json, "max_lines", &ml);
			rss_json_get_int(cmd_json, "max_line_length", &mll);
			if (ml > 0 && ml <= ROD_RECEIPT_MAX_LINES)
				e->receipt.max_lines = ml;
			if (mll > 0 && mll <= ROD_RECEIPT_MAX_LINE)
				e->receipt.max_line_len = mll;
		}
		for (int s = 0; s < st->stream_count; s++) {
			int fs = e->font_size > 0 ? e->font_size : st->settings.font_size;
			if (s > 0 && st->stream_h[0] > 0) {
				fs = fs * st->stream_h[s] / st->stream_h[0];
				if (fs < 12)
					fs = 12;
			}
			int fi = rod_alloc_font(st, s, fs);
			if (fi >= 0) {
				e->streams[s].font_idx = fi;
				create_elem_shm(st, e, s);
			}
		}
	}

	for (int s = 0; s < st->stream_count; s++) {
		char fwd[128];
		cJSON *j = cJSON_CreateObject();
		if (!j)
			continue;
		cJSON_AddStringToObject(j, "cmd", "osd-position");
		cJSON_AddNumberToObject(j, "stream", s);
		cJSON_AddStringToObject(j, "region", name);
		cJSON_AddStringToObject(j, "pos", position);
		cJSON_PrintPreallocated(j, fwd, sizeof(fwd), 0);
		cJSON_Delete(j);
		char rvd_resp[256];
		rss_ctrl_send_command(ROD_RVD_SOCK, fwd, rvd_resp, sizeof(rvd_resp), 1000);
	}
	RSS_INFO("add-element: %s type=%s template=\"%s\" pos=%s", name, type_str, tmpl, position);
	return rss_ctrl_resp_ok(resp, resp_size);
}

static int handle_remove_element(rod_state_t *st, const char *cmd_json, char *resp, int resp_size)
{
	char name[ROD_ELEM_NAME_LEN] = "";
	rss_json_get_str(cmd_json, "name", name, sizeof(name));
	if (!name[0])
		return rss_ctrl_resp_error(resp, resp_size, "need name");

	if (!rod_find_element(st, name))
		return rss_ctrl_resp_error(resp, resp_size, "not found");

	rod_remove_element(st, name);
	RSS_INFO("remove-element: %s", name);
	return rss_ctrl_resp_ok(resp, resp_size);
}

static int handle_set_element(rod_state_t *st, const char *cmd_json, char *resp, int resp_size)
{
	char name[ROD_ELEM_NAME_LEN] = "";
	rss_json_get_str(cmd_json, "name", name, sizeof(name));
	if (!name[0])
		return rss_ctrl_resp_error(resp, resp_size, "need name");

	rod_element_t *e = rod_find_element(st, name);
	if (!e)
		return rss_ctrl_resp_error(resp, resp_size, "not found");

	char val[ROD_TMPL_LEN];
	if (rss_json_get_str(cmd_json, "template", val, sizeof(val)) == 0) {
		sanitize_text(val);
		rss_strlcpy(e->tmpl, val, sizeof(e->tmpl));
		e->last_expanded[0] = '\0';
		mark_element_dirty(e, st->stream_count);
	}

	if (rss_json_get_str(cmd_json, "position", val, sizeof(val)) == 0) {
		rss_strlcpy(e->position, val, sizeof(e->position));
		for (int s = 0; s < st->stream_count; s++) {
			char fwd[128];
			cJSON *j = cJSON_CreateObject();
			if (!j)
				continue;
			cJSON_AddStringToObject(j, "cmd", "osd-position");
			cJSON_AddNumberToObject(j, "stream", s);
			cJSON_AddStringToObject(j, "region", name);
			cJSON_AddStringToObject(j, "pos", val);
			cJSON_PrintPreallocated(j, fwd, sizeof(fwd), 0);
			cJSON_Delete(j);
			char rvd_resp[256];
			rss_ctrl_send_command(ROD_RVD_SOCK, fwd, rvd_resp, sizeof(rvd_resp), 1000);
		}
	}

	int ival;
	if (rss_json_get_int(cmd_json, "align", &ival) == 0 && ival >= 0 && ival <= 2) {
		e->align = ival;
		mark_element_dirty(e, st->stream_count);
	}

	if (rss_json_get_str(cmd_json, "color", val, sizeof(val)) == 0) {
		e->color = (uint32_t)strtoul(val, NULL, 0);
		e->has_color = true;
		mark_element_dirty(e, st->stream_count);
	}

	if (rss_json_get_str(cmd_json, "stroke_color", val, sizeof(val)) == 0) {
		e->stroke_color = (uint32_t)strtoul(val, NULL, 0);
		e->has_stroke_color = true;
		mark_element_dirty(e, st->stream_count);
	}

	if (rss_json_get_int(cmd_json, "stroke_size", &ival) == 0 && ival >= 0 && ival <= 5) {
		e->stroke_size = ival;
		mark_element_dirty(e, st->stream_count);
	}

	int new_font_size = 0;
	if (rss_json_get_int(cmd_json, "font_size", &new_font_size) == 0 && new_font_size >= 10 &&
	    new_font_size <= 72 && new_font_size != e->font_size &&
	    (e->type == ROD_ELEM_TEXT || e->type == ROD_ELEM_RECEIPT)) {
		e->font_size = new_font_size;
		for (int s = 0; s < st->stream_count; s++) {
			int fs = new_font_size;
			if (s > 0 && st->stream_h[0] > 0) {
				fs = fs * st->stream_h[s] / st->stream_h[0];
				if (fs < 12)
					fs = 12;
			}
			release_font(st, s, e->streams[s].font_idx);
			int fi = rod_alloc_font(st, s, fs);
			if (fi < 0) {
				RSS_ERROR("font alloc failed s%d elem %s", s, e->name);
				e->streams[s].font_idx = -1;
				continue;
			}
			e->streams[s].font_idx = fi;
			if (e->streams[s].shm) {
				rss_osd_destroy(e->streams[s].shm);
				e->streams[s].shm = NULL;
			}
			create_elem_shm(st, e, s);
		}
		notify_rvd_osd_restart(st);
	}

	return rss_ctrl_resp_ok(resp, resp_size);
}

static int handle_set_var(rod_state_t *st, const char *cmd_json, char *resp, int resp_size)
{
	char name[32] = "", value[128] = "";
	rss_json_get_str(cmd_json, "name", name, sizeof(name));
	rss_json_get_str(cmd_json, "value", value, sizeof(value));
	if (!name[0])
		return rss_ctrl_resp_error(resp, resp_size, "need name");

	sanitize_text(value);

	for (int i = 0; i < st->var_count; i++) {
		if (strcmp(st->vars[i].name, name) == 0) {
			rss_strlcpy(st->vars[i].value, value, sizeof(st->vars[i].value));
			return rss_ctrl_resp_ok(resp, resp_size);
		}
	}
	if (st->var_count >= ROD_MAX_VARS)
		return rss_ctrl_resp_error(resp, resp_size, "var limit reached");

	rss_strlcpy(st->vars[st->var_count].name, name, sizeof(st->vars[0].name));
	rss_strlcpy(st->vars[st->var_count].value, value, sizeof(st->vars[0].value));
	st->var_count++;
	return rss_ctrl_resp_ok(resp, resp_size);
}

static int handle_receipt(rod_state_t *st, const char *cmd_json, char *resp, int resp_size)
{
	char name[ROD_ELEM_NAME_LEN] = "";
	char text[ROD_RECEIPT_MAX_LINE + 1] = "";
	rss_json_get_str(cmd_json, "name", name, sizeof(name));
	rss_json_get_str(cmd_json, "text", text, sizeof(text));

	rod_element_t *e = NULL;
	if (name[0]) {
		e = rod_find_element(st, name);
	} else {
		for (int i = 0; i < st->elem_count; i++) {
			if (st->elements[i].active && st->elements[i].type == ROD_ELEM_RECEIPT) {
				e = &st->elements[i];
				break;
			}
		}
	}
	if (!e || e->type != ROD_ELEM_RECEIPT)
		return rss_ctrl_resp_error(resp, resp_size, "no receipt element");

	if (text[0])
		rod_receipt_add_line(e, text);
	return rss_ctrl_resp_ok(resp, resp_size);
}

static int handle_receipt_clear(rod_state_t *st, const char *cmd_json, char *resp, int resp_size)
{
	char name[ROD_ELEM_NAME_LEN] = "";
	rss_json_get_str(cmd_json, "name", name, sizeof(name));

	rod_element_t *e = NULL;
	if (name[0]) {
		e = rod_find_element(st, name);
	} else {
		for (int i = 0; i < st->elem_count; i++) {
			if (st->elements[i].active && st->elements[i].type == ROD_ELEM_RECEIPT) {
				e = &st->elements[i];
				break;
			}
		}
	}
	if (!e || e->type != ROD_ELEM_RECEIPT)
		return rss_ctrl_resp_error(resp, resp_size, "no receipt element");

	rod_receipt_clear(e);
	return rss_ctrl_resp_ok(resp, resp_size);
}

static int handle_show_hide(rod_state_t *st, const char *cmd_json, char *resp, int resp_size,
			    bool show)
{
	char name[ROD_ELEM_NAME_LEN] = "";
	rss_json_get_str(cmd_json, "name", name, sizeof(name));
	if (!name[0])
		return rss_ctrl_resp_error(resp, resp_size, "need name");

	rod_element_t *e = rod_find_element(st, name);
	if (!e)
		return rss_ctrl_resp_error(resp, resp_size, "not found");

	e->visible = show;

	for (int s = 0; s < st->stream_count; s++) {
		char fwd[128];
		cJSON *j = cJSON_CreateObject();
		if (!j)
			continue;
		cJSON_AddStringToObject(j, "cmd", "osd-show");
		cJSON_AddNumberToObject(j, "stream", s);
		cJSON_AddStringToObject(j, "region", name);
		cJSON_AddNumberToObject(j, "show", show ? 1 : 0);
		cJSON_PrintPreallocated(j, fwd, sizeof(fwd), 0);
		cJSON_Delete(j);
		char rvd_resp[256];
		rss_ctrl_send_command(ROD_RVD_SOCK, fwd, rvd_resp, sizeof(rvd_resp), 1000);
	}
	return rss_ctrl_resp_ok(resp, resp_size);
}

int rod_ctrl_handler(const char *cmd_json, char *resp_buf, int resp_buf_size, void *userdata)
{
	rod_state_t *st = userdata;

	int rc =
		rss_ctrl_handle_common(cmd_json, resp_buf, resp_buf_size, st->cfg, st->config_path);
	if (rc >= 0)
		return rc;

	char cmd[64];
	if (rss_json_get_str(cmd_json, "cmd", cmd, sizeof(cmd)) != 0)
		return rss_ctrl_resp_error(resp_buf, resp_buf_size, "missing cmd");

	if (strcmp(cmd, "set-font-color") == 0)
		return handle_color_change(st, cmd_json, resp_buf, resp_buf_size, "font_color",
					   &st->settings.font_color);

	if (strcmp(cmd, "set-stroke-color") == 0)
		return handle_color_change(st, cmd_json, resp_buf, resp_buf_size, "stroke_color",
					   &st->settings.stroke_color);

	if (strcmp(cmd, "set-stroke-size") == 0) {
		int val;
		if (rss_json_get_int(cmd_json, "value", &val) != 0 || val < 0 || val > 5)
			return rss_ctrl_resp_error(resp_buf, resp_buf_size, "need value 0-5");
		st->settings.font_stroke = val;
		rss_config_set_int(st->cfg, "osd", "font_stroke", val);
		mark_all_dirty(st);
		return rss_ctrl_resp_ok(resp_buf, resp_buf_size);
	}

	if (strcmp(cmd, "set-position") == 0)
		return handle_set_position(st, cmd_json, resp_buf, resp_buf_size);

	if (strcmp(cmd, "set-font-size") == 0)
		return handle_font_size_change(st, cmd_json, resp_buf, resp_buf_size);

	if (strcmp(cmd, "elements") == 0)
		return handle_elements_list(st, resp_buf, resp_buf_size);

	if (strcmp(cmd, "add-element") == 0)
		return handle_add_element(st, cmd_json, resp_buf, resp_buf_size);

	if (strcmp(cmd, "remove-element") == 0)
		return handle_remove_element(st, cmd_json, resp_buf, resp_buf_size);

	if (strcmp(cmd, "set-element") == 0)
		return handle_set_element(st, cmd_json, resp_buf, resp_buf_size);

	if (strcmp(cmd, "set-var") == 0)
		return handle_set_var(st, cmd_json, resp_buf, resp_buf_size);

	if (strcmp(cmd, "receipt") == 0)
		return handle_receipt(st, cmd_json, resp_buf, resp_buf_size);

	if (strcmp(cmd, "receipt-clear") == 0)
		return handle_receipt_clear(st, cmd_json, resp_buf, resp_buf_size);

	if (strcmp(cmd, "show-element") == 0)
		return handle_show_hide(st, cmd_json, resp_buf, resp_buf_size, true);

	if (strcmp(cmd, "hide-element") == 0)
		return handle_show_hide(st, cmd_json, resp_buf, resp_buf_size, false);

	if (strcmp(cmd, "config-show") == 0) {
		char fc[16], sc[16];
		snprintf(fc, sizeof(fc), "0x%08X", st->settings.font_color);
		snprintf(sc, sizeof(sc), "0x%08X", st->settings.stroke_color);
		cJSON *r = cJSON_CreateObject();
		if (!r)
			return rss_ctrl_resp_error(resp_buf, resp_buf_size, "alloc");
		cJSON_AddStringToObject(r, "status", "ok");
		cJSON *cfg = cJSON_AddObjectToObject(r, "config");
		cJSON_AddNumberToObject(cfg, "font_size", st->settings.font_size);
		cJSON_AddStringToObject(cfg, "font_color", fc);
		cJSON_AddStringToObject(cfg, "stroke_color", sc);
		cJSON_AddNumberToObject(cfg, "font_stroke", st->settings.font_stroke);
		cJSON_AddNumberToObject(cfg, "elements", st->elem_count);
		return rss_ctrl_resp_json(resp_buf, resp_buf_size, r);
	}

	/* Default: status */
	int region_count = 0;
	for (int i = 0; i < st->elem_count; i++) {
		if (!st->elements[i].active)
			continue;
		for (int s = 0; s < st->stream_count; s++) {
			if (st->elements[i].streams[s].shm)
				region_count++;
		}
	}

	cJSON *r = cJSON_CreateObject();
	if (!r)
		return rss_ctrl_resp_error(resp_buf, resp_buf_size, "alloc");
	cJSON_AddStringToObject(r, "status", "ok");
	cJSON_AddNumberToObject(r, "streams", st->stream_count);
	cJSON_AddNumberToObject(r, "elements", st->elem_count);
	cJSON_AddNumberToObject(r, "regions", region_count);
	return rss_ctrl_resp_json(resp_buf, resp_buf_size, r);
}

/*
 * rsd555_ctrl.c -- Control socket handler
 */

#include "rsd555.h"

int rsd555_ctrl_handler(const char *cmd_json, char *resp_buf, int resp_buf_size, void *userdata)
{
	rsd555_state_t *st = userdata;

	int rc = rss_ctrl_handle_common(cmd_json, resp_buf, resp_buf_size, st->cfg,
					st->config_path);
	if (rc >= 0)
		return rc;

	char cmd[64];
	if (rss_json_get_str(cmd_json, "cmd", cmd, sizeof(cmd)) != 0)
		return rss_ctrl_resp_error(resp_buf, resp_buf_size, "missing cmd");

	if (strcmp(cmd, "config-show") == 0) {
		cJSON *r = cJSON_CreateObject();
		cJSON_AddStringToObject(r, "status", "ok");
		cJSON *cfg = cJSON_AddObjectToObject(r, "config");
		cJSON_AddNumberToObject(cfg, "port", st->port);
		cJSON_AddNumberToObject(cfg, "max_clients", st->max_clients);
		cJSON_AddNumberToObject(cfg, "session_timeout", st->session_timeout);
		cJSON_AddStringToObject(cfg, "session_name", st->session_name);
		cJSON_AddStringToObject(cfg, "config_path", st->config_path);
		return rss_ctrl_resp_json(resp_buf, resp_buf_size, r);
	}

	if (strcmp(cmd, "status") == 0) {
		cJSON *r = cJSON_CreateObject();
		cJSON_AddStringToObject(r, "status", "ok");
		cJSON_AddNumberToObject(r, "port", st->port);

		cJSON *streams = cJSON_AddArrayToObject(r, "streams");
		for (int s = 0; s < RSD555_STREAM_COUNT; s++) {
			if (st->video[s].width == 0)
				continue;
			cJSON *item = cJSON_CreateObject();
			cJSON_AddNumberToObject(item, "index", s);
			cJSON_AddStringToObject(item, "ring", rsd555_ring_names[s]);
			cJSON_AddStringToObject(item, "codec",
						st->video[s].codec == 0 ? "h264" : "h265");
			cJSON_AddNumberToObject(item, "width", st->video[s].width);
			cJSON_AddNumberToObject(item, "height", st->video[s].height);
			pthread_mutex_lock(&st->video[s].sources_lock);
			int vc = st->video[s].source_count;
			pthread_mutex_unlock(&st->video[s].sources_lock);
			cJSON_AddNumberToObject(item, "clients", vc);
			cJSON_AddItemToArray(streams, item);
		}

		if (st->has_audio) {
			cJSON *au = cJSON_AddObjectToObject(r, "audio");
			cJSON_AddNumberToObject(au, "codec", st->audio.codec);
			cJSON_AddNumberToObject(au, "sample_rate", st->audio.sample_rate);
			pthread_mutex_lock(&st->audio.sources_lock);
			int ac = st->audio.source_count;
			pthread_mutex_unlock(&st->audio.sources_lock);
			cJSON_AddNumberToObject(au, "clients", ac);
		}

		return rss_ctrl_resp_json(resp_buf, resp_buf_size, r);
	}

	/* Default: basic status */
	cJSON *r = cJSON_CreateObject();
	cJSON_AddStringToObject(r, "status", "ok");
	cJSON_AddNumberToObject(r, "port", st->port);
	return rss_ctrl_resp_json(resp_buf, resp_buf_size, r);
}

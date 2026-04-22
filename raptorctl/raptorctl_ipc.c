/*
 * raptorctl_ipc.c -- JSON helpers, IPC transport, and JSON batch mode
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cJSON.h>
#include <rss_ipc.h>
#include <rss_common.h>

#include "raptorctl.h"

int is_daemon(const char *name)
{
	for (int i = 0; daemons[i]; i++) {
		if (strcmp(name, daemons[i]) == 0)
			return 1;
	}
	return 0;
}

cJSON *jcmd(const char *cmd)
{
	cJSON *j = cJSON_CreateObject();
	cJSON_AddStringToObject(j, "cmd", cmd);
	return j;
}

void jadd_s(cJSON *j, const char *key, const char *val)
{
	cJSON_AddStringToObject(j, key, val);
}

void jadd_auto(cJSON *j, const char *key, const char *val)
{
	if (val[0] == '0' && (val[1] == 'x' || val[1] == 'X')) {
		cJSON_AddStringToObject(j, key, val);
		return;
	}
	char *end;
	long lv = strtol(val, &end, 10);
	if (*end == '\0' && end != val)
		cJSON_AddNumberToObject(j, key, (double)lv);
	else
		cJSON_AddStringToObject(j, key, val);
}

void jadd_i(cJSON *j, const char *key, const char *val)
{
	cJSON_AddNumberToObject(j, key, (double)strtol(val, NULL, 10));
}

void jstr(cJSON *j, char *buf, int size)
{
	char *s = cJSON_PrintUnformatted(j);
	if (s) {
		rss_strlcpy(buf, s, (size_t)size);
		free(s);
	} else {
		buf[0] = '\0';
	}
	cJSON_Delete(j);
}

int send_cmd(const char *daemon, const char *json)
{
	char sock_path[64];
	snprintf(sock_path, sizeof(sock_path), RSS_SOCK_FMT, daemon);

	char resp[2048];
	int ret = rss_ctrl_send_command(sock_path, json, resp, sizeof(resp), 5000);
	if (ret < 0) {
		fprintf(stderr, "Failed to send to %s: %s\n", daemon,
			ret == -2 ? "timeout" : "connection failed");
		return 1;
	}

	printf("%s\n", resp);
	return 0;
}

int send_cmd_json(const char *daemon, const char *json, char *resp, int resp_size)
{
	char sock_path[64];
	snprintf(sock_path, sizeof(sock_path), RSS_SOCK_FMT, daemon);

	int ret = rss_ctrl_send_command(sock_path, json, resp, resp_size, 5000);
	if (ret < 0) {
		snprintf(resp, (size_t)resp_size, "{\"status\":\"error\",\"reason\":\"%s\"}",
			 ret == -2 ? "timeout" : "connection failed");
	}
	return ret < 0 ? 1 : 0;
}

static cJSON *json_error(const char *reason)
{
	cJSON *e = cJSON_CreateObject();
	cJSON_AddStringToObject(e, "status", "error");
	cJSON_AddStringToObject(e, "reason", reason);
	return e;
}

static int run_json_cmd(cJSON *item, cJSON *results)
{
	cJSON *daemon_obj = cJSON_DetachItemFromObjectCaseSensitive(item, "daemon");
	if (!daemon_obj || !cJSON_IsString(daemon_obj)) {
		cJSON_AddItemToArray(results, json_error("missing daemon"));
		cJSON_Delete(daemon_obj);
		return 1;
	}

	const char *daemon = daemon_obj->valuestring;
	if (!is_daemon(daemon)) {
		cJSON_AddItemToArray(results, json_error("unknown daemon"));
		cJSON_Delete(daemon_obj);
		return 1;
	}

	char *payload = cJSON_PrintUnformatted(item);
	if (!payload) {
		cJSON_AddItemToArray(results, json_error("out of memory"));
		cJSON_Delete(daemon_obj);
		return 1;
	}

	char resp[2048];
	int ret = send_cmd_json(daemon, payload, resp, sizeof(resp));
	free(payload);
	cJSON_Delete(daemon_obj);

	cJSON *parsed = cJSON_Parse(resp);
	cJSON_AddItemToArray(results, parsed ? parsed : cJSON_CreateString(resp));
	return ret;
}

int handle_json_mode(const char *input)
{
	cJSON *root = cJSON_Parse(input);
	if (!root) {
		fprintf(stderr, "Invalid JSON\n");
		return 1;
	}

	cJSON *results = cJSON_CreateArray();
	int errors = 0;

	if (cJSON_IsArray(root)) {
		cJSON *item = NULL;
		cJSON_ArrayForEach(item, root) errors += run_json_cmd(item, results);
	} else if (cJSON_IsObject(root)) {
		errors = run_json_cmd(root, results);
	} else {
		fprintf(stderr, "JSON must be an object or array\n");
		cJSON_Delete(root);
		cJSON_Delete(results);
		return 1;
	}

	/* Single command: print response directly; batch: print array */
	char *s;
	if (cJSON_GetArraySize(results) == 1 && !cJSON_IsArray(root))
		s = cJSON_PrintUnformatted(cJSON_GetArrayItem(results, 0));
	else
		s = cJSON_PrintUnformatted(results);

	if (s) {
		printf("%s\n", s);
		free(s);
	} else {
		fprintf(stderr, "Failed to format response\n");
		errors = 1;
	}

	cJSON_Delete(root);
	cJSON_Delete(results);
	return errors ? 1 : 0;
}

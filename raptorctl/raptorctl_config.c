/*
 * raptorctl_config.c -- Config get/set/save subcommand
 */

#include <stdio.h>
#include <string.h>

#include <cJSON.h>
#include <rss_ipc.h>
#include <rss_common.h>

#include "raptorctl.h"

static const char *find_daemon_for_section(const char *section)
{
	static const struct {
		const char *section;
		const char *daemon;
	} map[] = {
		{"sensor", "rvd"},     {"stream0", "rvd"},    {"stream1", "rvd"},
		{"jpeg", "rvd"},       {"ring", "rvd"},	      {"audio", "rad"},
		{"rtsp", "rsd"},       {"http", "rhd"},	      {"osd", "rod"},
		{"ircut", "ric"},      {"recording", "rmr"},  {"motion", "rmd"},
		{"webrtc", "rwd"},     {"webtorrent", "rwd"}, {"webcam", "rwc"},
		{"filesource", "rfs"}, {"log", "rvd"},	      {NULL, NULL},
	};
	for (int i = 0; map[i].section; i++) {
		if (strcmp(section, map[i].section) == 0)
			return map[i].daemon;
	}
	return NULL;
}

static void print_config_entry(const char *key, const char *value, void *userdata)
{
	int *count = userdata;
	printf("%s = %s\n", key, value);
	(*count)++;
}

static void print_section_json(const char *section, const char *json_str)
{
	printf("[%s]\n", section);
	cJSON *root = cJSON_Parse(json_str);
	if (!root)
		return;
	cJSON *keys_obj = cJSON_GetObjectItemCaseSensitive(root, "keys");
	if (!keys_obj) {
		cJSON_Delete(root);
		return;
	}
	cJSON *item = NULL;
	cJSON_ArrayForEach(item, keys_obj)
	{
		const char *v = cJSON_GetStringValue(item);
		if (v)
			printf("%s = %s\n", item->string, v);
	}
	cJSON_Delete(root);
}

int handle_config(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "Usage: raptorctl config <get|set|save> ...\n");
		return 1;
	}

	/* config save — tell all daemons to save */
	if (strcmp(argv[2], "save") == 0) {
		int saved = 0;
		for (int i = 0; daemons[i]; i++) {
			char sock_path[64];
			char resp[2048];
			snprintf(sock_path, sizeof(sock_path), "/var/run/rss/%s.sock", daemons[i]);
			int ret = rss_ctrl_send_command(sock_path, "{\"cmd\":\"config-save\"}",
							resp, sizeof(resp), 2000);
			if (ret >= 0) {
				printf("%s: %s\n", daemons[i], resp);
				saved++;
			}
		}
		if (saved == 0) {
			fprintf(stderr, "No daemons responded\n");
			return 1;
		}
		return 0;
	}

	/* config get <section> [key] */
	if (strcmp(argv[2], "get") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl config get <section> [key]\n");
			return 1;
		}
		const char *section = argv[3];
		const char *key = argc >= 5 ? argv[4] : NULL;

		/* Try daemon first for single-key get */
		if (key) {
			const char *target = find_daemon_for_section(section);
			if (target) {
				char sock_path[64];
				char resp[2048];
				char cmd_json[256];
				snprintf(sock_path, sizeof(sock_path), "/var/run/rss/%s.sock",
					 target);
				cJSON *j = jcmd("config-get");
				if (!j)
					return 1;
				jadd_s(j, "section", section);
				jadd_s(j, "key", key);
				jstr(j, cmd_json, sizeof(cmd_json));
				int ret = rss_ctrl_send_command(sock_path, cmd_json, resp,
								sizeof(resp), 2000);
				if (ret >= 0) {
					printf("%s\n", resp);
					return 0;
				}
			}
		}

		if (!key) {
			/* Section dump: try daemon first */
			const char *target = find_daemon_for_section(section);
			if (target) {
				char sock_path[64];
				char resp[2048];
				char cmd_json[256];
				snprintf(sock_path, sizeof(sock_path), "/var/run/rss/%s.sock",
					 target);
				cJSON *j = jcmd("config-get-section");
				if (!j)
					return 1;
				jadd_s(j, "section", section);
				jstr(j, cmd_json, sizeof(cmd_json));
				int ret = rss_ctrl_send_command(sock_path, cmd_json, resp,
								sizeof(resp), 2000);
				if (ret >= 0) {
					print_section_json(section, resp);
					return 0;
				}
			}
		}

		/* Fallback: read from config file */
		const char *cfgpath = "/etc/raptor.conf";
		rss_config_t *cfg = rss_config_load(cfgpath);
		if (!cfg) {
			fprintf(stderr, "Config not found\n");
			return 1;
		}

		if (key) {
			const char *val = rss_config_get_str(cfg, section, key, NULL);
			if (val)
				printf("%s\n", val);
			else
				fprintf(stderr, "Key not found: [%s] %s\n", section, key);
			rss_config_free(cfg);
			return val ? 0 : 1;
		}

		/* Section dump from file */
		printf("[%s]\n", section);
		int count = 0;
		rss_config_foreach(cfg, section, print_config_entry, &count);
		if (count == 0)
			fprintf(stderr, "Section not found: [%s]\n", section);
		rss_config_free(cfg);
		return count > 0 ? 0 : 1;
	}

	/* config set <section> <key> <value> */
	if (strcmp(argv[2], "set") == 0) {
		if (argc < 6) {
			fprintf(stderr, "Usage: raptorctl config set <section> <key> <value>\n");
			return 1;
		}
		const char *cfgpath = "/etc/raptor.conf";
		rss_config_t *cfg = rss_config_load(cfgpath);
		if (!cfg) {
			fprintf(stderr, "Failed to load %s\n", cfgpath);
			return 1;
		}
		rss_config_set_str(cfg, argv[3], argv[4], argv[5]);
		int ret = rss_config_save(cfg, cfgpath);
		rss_config_free(cfg);
		if (ret != 0) {
			fprintf(stderr, "Failed to save %s\n", cfgpath);
			return 1;
		}
		return 0;
	}

	fprintf(stderr, "Usage: raptorctl config <get|set|save> ...\n");
	return 1;
}

/*
 * raptorctl.c -- Raptor control CLI
 *
 * Usage:
 *   raptorctl status                       Check which daemons are running
 *   raptorctl config get <section> [key]   Read config value(s)
 *   raptorctl config set <section> <key> <value>  Set a config value
 *   raptorctl config save                  Save running config to disk
 *   raptorctl <daemon> <command> [args]    Send command to daemon
 *
 * RVD commands:
 *   raptorctl rvd status                   Show encoder channel stats
 *   raptorctl rvd config                   Show running config
 *   raptorctl rvd request-idr [channel]    Request keyframe
 *   raptorctl rvd set-bitrate <ch> <bps>   Change bitrate
 *   raptorctl rvd set-gop <ch> <length>    Change GOP length
 *   raptorctl rvd set-fps <ch> <fps>       Change frame rate
 *   raptorctl rvd set-qp-bounds <ch> <min> <max>  Change QP range
 *   raptorctl rvd get-enc-caps             Show encoder capabilities
 *   ... plus ~30 advanced encoder commands (see --help)
 *
 * RAD commands:
 *   raptorctl rad status                   Show audio status
 *   raptorctl rad config                   Show running config
 *   raptorctl rad set-volume <val>         Change input volume
 *   raptorctl rad set-gain <val>           Change input gain
 *   raptorctl rad set-ns <0|1> [level]    Noise suppression on/off
 *   raptorctl rad set-hpf <0|1>           High-pass filter on/off
 *   raptorctl rad set-agc <0|1> [t] [c]   AGC on/off
 *
 * RSD commands:
 *   raptorctl rsd status                   Show client count
 *   raptorctl rsd config                   Show running config
 *
 * ROD commands:
 *   raptorctl rod status                   Show OSD element status
 *   raptorctl rod elements                 List all OSD elements
 *   raptorctl rod add-element <n> [k=v]    Create OSD element
 *   raptorctl rod set-element <n> [k=v]    Modify element property
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cJSON.h>
#include <rss_ipc.h>
#include <rss_common.h>

#include "raptorctl.h"

const char *daemons[] = {"rvd", "rsd", "rad", "rod", "rhd", "ric",
			 "rmr", "rmd", "rwd", "rwc", "rfs", NULL};

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

/* JSON command helpers for building IPC requests */
static cJSON *jcmd(const char *cmd)
{
	cJSON *j = cJSON_CreateObject();
	cJSON_AddStringToObject(j, "cmd", cmd);
	return j;
}

static void jadd_s(cJSON *j, const char *key, const char *val)
{
	cJSON_AddStringToObject(j, key, val);
}

static void jadd_auto(cJSON *j, const char *key, const char *val)
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

static void jadd_i(cJSON *j, const char *key, const char *val)
{
	cJSON_AddNumberToObject(j, key, (double)strtol(val, NULL, 10));
}

static void jstr(cJSON *j, char *buf, int size)
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

/* Pretty-print a config-get-section JSON response as ini-style key = value.
 * Input: {"status":"ok","section":"audio","keys":{"codec":"aac","volume":"80"}}
 * Output: [audio]
 *         codec = aac
 *         volume = 80 */
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

const struct help_entry help_entries[] = {
	{NULL, "status                              Show daemon status"},
	{NULL, "memory                              Show memory usage (private/shared)"},
	{NULL, "cpu                                 Show CPU usage (1s sample)"},
	{NULL, "config get <section> <key>           Read config value"},
	{NULL, "config get <section>                Show all keys in section"},
	{NULL, "config set <section> <key> <value>  Set config value"},
	{NULL, "config save                         Save running config to disk"},
	{NULL, "<daemon> status                     Show daemon details"},
	{NULL, "<daemon> config                     Show running config"},
	{NULL, "<daemon> <cmd> [args...]            Send command"},
	{"rvd", "set-rc-mode <ch> <mode> [bps]       Change rate control mode"},
	{"rvd", "set-bitrate <ch> <bps>              Change bitrate"},
	{"rvd", "set-gop <ch> <length>               Change GOP length"},
	{"rvd", "set-fps <ch> <fps>                  Change frame rate"},
	{"rvd", "set-qp-bounds <ch> <min> <max>      Change QP range"},
	{"rvd", "set-qp <ch> <qp>                   Set fixed QP (all frames)"},
	{"rvd", "set-qp-ip-delta <ch> <delta>        I/P frame QP delta"},
	{"rvd", "set-qp-bounds-per-frame <ch> ...    Per-frame QP (iMin iMax pMin pMax)"},
	{"rvd", "set-gop-mode <ch> <0|1|2>           GOP mode (0=def 1=pyr 2=smartP)"},
	{"rvd", "set-rc-options <ch> <bitmask>       RC options bitmask"},
	{"rvd", "set-max-same-scene <ch> <count>     Max same-scene count"},
	{"rvd", "set-max-pic-size <ch> <iK> <pK>     Max I/P frame size (kbits)"},
	{"rvd", "set-color2grey <ch> <0|1>           Color to greyscale"},
	{"rvd", "set-mbrc <ch> <0|1>                 Macroblock rate control"},
	{"rvd", "set-entropy-mode <ch> <0|1>         0=CAVLC 1=CABAC"},
	{"rvd", "set-resize-mode <ch> <0|1>          Resize mode"},
	{"rvd", "set-stream-buf-size <ch> <bytes>    Stream buffer size"},
	{"rvd", "set-qpg-mode <ch> <mode>            QPG mode"},
	{"rvd", "set-h264-trans <ch> <offset>        H.264 chroma QP offset [-12..12]"},
	{"rvd", "set-h265-trans <ch> <cr> <cb>       H.265 chroma QP offsets [-12..12]"},
	{"rvd", "set-roi <ch> <idx> ...              ROI region (en x y w h qp)"},
	{"rvd", "set-super-frame <ch> <mode> ...     Super frame (mode iThr pThr)"},
	{"rvd", "set-pskip <ch> <en> <maxf>          P-skip (enable max_frames)"},
	{"rvd", "set-srd <ch> <en> <level>           Static refresh (enable level)"},
	{"rvd", "set-enc-denoise <ch> ...             Encoder denoise (en type iQP pQP)"},
	{"rvd", "set-gdr <ch> <en> <cycle>           GDR (enable cycle)"},
	{"rvd", "set-enc-crop <ch> <en> <x y w h>    Encoder crop"},
	{"rvd", "set-jpeg-qp <ch> <qp>              JPEG QP"},
	{"rvd", "get-enc-caps                        Show encoder capabilities"},
	{"rvd", "get-gop-mode <ch>                   Show GOP mode"},
	{"rvd", "get-rc-options <ch>                 Show RC options"},
	{"rvd", "get-max-same-scene <ch>             Show max same-scene count"},
	{"rvd", "get-color2grey <ch>                 Show color2grey state"},
	{"rvd", "get-mbrc <ch>                       Show macroblock RC state"},
	{"rvd", "get-qpg-mode <ch>                   Show QPG mode"},
	{"rvd", "get-stream-buf-size <ch>            Show stream buffer size"},
	{"rvd", "get-h264-trans <ch>                 Show H.264 chroma QP offset"},
	{"rvd", "get-h265-trans <ch>                 Show H.265 chroma QP offsets"},
	{"rvd", "get-roi <ch> <idx>                  Show ROI region"},
	{"rvd", "get-super-frame <ch>                Show super frame config"},
	{"rvd", "get-pskip <ch>                      Show P-skip config"},
	{"rvd", "get-srd <ch>                        Show SRD config"},
	{"rvd", "get-enc-denoise <ch>                Show encoder denoise config"},
	{"rvd", "get-gdr <ch>                        Show GDR config"},
	{"rvd", "get-enc-crop <ch>                   Show encoder crop"},
	{"rvd", "get-jpeg-qp <ch>                    Show JPEG QP"},
	{"rvd", "get-bitrate <ch>                    Show target + avg bitrate"},
	{"rvd", "get-fps <ch>                        Show frame rate"},
	{"rvd", "get-gop <ch>                        Show GOP length"},
	{"rvd", "get-qp-bounds <ch>                  Show QP range"},
	{"rvd", "get-rc-mode <ch>                    Show rate control mode"},
	{"rvd", "request-idr [channel]               Request keyframe"},
	{"rvd", "stream-stop <ch>                    Stop stream pipeline"},
	{"rvd", "stream-start <ch>                   Start stopped stream"},
	{"rvd", "stream-restart <ch>                 Restart stream pipeline"},
	{"rvd", "set-codec <ch> <h264|h265>          Change codec (requires restart)"},
	{"rvd", "set-resolution <ch> <w> <h>         Change resolution (requires restart)"},
	{"rvd", "request-pskip <ch>                  Request P-skip"},
	{"rvd", "request-gdr <ch> <frames>           Request GDR"},
	{"rvd", "set-brightness <val>                ISP brightness (0-255)"},
	{"rvd", "set-contrast <val>                  ISP contrast (0-255)"},
	{"rvd", "set-saturation <val>                ISP saturation (0-255)"},
	{"rvd", "set-sharpness <val>                 ISP sharpness (0-255)"},
	{"rvd", "set-hue <val>                       ISP hue (0-255)"},
	{"rvd", "set-sinter <val>                    Spatial NR (0-255)"},
	{"rvd", "set-temper <val>                    Temporal NR (0-255)"},
	{"rvd", "set-hflip <0|1>                     Horizontal flip"},
	{"rvd", "set-vflip <0|1>                     Vertical flip"},
	{"rvd", "set-antiflicker <0|1|2>             Off/50Hz/60Hz"},
	{"rvd", "set-ae-comp <val>                   AE compensation"},
	{"rvd", "set-max-again <val>                 Max analog gain"},
	{"rvd", "set-max-dgain <val>                 Max digital gain"},
	{"rvd", "set-defog <0|1>                     Defog enable"},
	{"rvd", "set-wb <mode> [r] [b]               White balance"},
	{"rvd", "get-wb                              Show white balance settings"},
	{"rvd", "get-isp                             Show all ISP settings"},
	{"rvd", "get-exposure                        Show exposure info"},
	{"rsd", "clients                             List connected clients"},
	{"rad", "set-codec <codec>                   Change audio codec (restart)"},
	{"rad", "set-volume <val>                    Input volume"},
	{"rad", "set-gain <val>                      Input gain"},
	{"rad", "set-alc-gain <0-7>                  ALC gain (T21/T31 only)"},
	{"rad", "mute                                Mute audio input"},
	{"rad", "unmute                              Unmute audio input"},
	{"rad", "set-ns <0|1> [0-3]                  Noise suppression level"},
	{"rad", "set-hpf <0|1>                       High-pass filter"},
	{"rad", "set-agc <0|1> [target] [comp]       Automatic gain control"},
	{"rad", "ao-set-volume <val>                 Speaker volume"},
	{"rad", "ao-set-gain <val>                   Speaker gain"},
	{"rad", "ao-mute                             Mute speaker (soft fade)"},
	{"rad", "ao-unmute                           Unmute speaker (soft fade)"},
	{"rod", "privacy [on|off] [channel]          Toggle privacy mode"},
	{"rod", "elements                            List all OSD elements"},
	{"rod", "add-element <name> [key=val]...     Create OSD element"},
	{"rod", "remove-element <name>               Remove OSD element"},
	{"rod", "set-element <name> [key=val]...     Modify element property"},
	{"rod", "show-element <name>                 Show element"},
	{"rod", "hide-element <name>                 Hide element"},
	{"rod", "set-var <name> <value>              Set template variable"},
	{"rod", "set-position <elem> <pos>           Move element (named or x,y)"},
	{"rod", "set-font-size <10-72>               Global font size"},
	{"rod", "set-font-color <0xAARRGGBB>         Global text color"},
	{"rod", "set-stroke-color <0xAARRGGBB>       Global stroke color"},
	{"rod", "set-stroke-size <0-5>               Global stroke width"},
	{"ric", "mode <auto|day|night>               Set day/night mode (GPIO + ISP)"},
	{"ric", "isp-mode <day|night>                Set ISP mode only (no GPIO)"},
	{"rhd", "clients                             List connected clients"},
	{"rwd", "clients                             List connected clients"},
	{"rwd", "share                               Show WebTorrent share URL"},
	{"rwd", "share-rotate                        Generate new share key"},
	{"rmd", "sensitivity <0-4>                   Set motion sensitivity"},
	{"rmd", "skip-frames <N>                      Set IVS skip frame count"},
	{NULL, "test-motion [sec]                   Trigger clip recording (default 10s)"},
	{NULL, NULL}};

static int same_section(const char *a, const char *b)
{
	if (a == b)
		return 1;
	if (!a || !b)
		return 0;
	return strcmp(a, b) == 0;
}

static void daemon_help(const char *name)
{
	printf("\nCommands:\n"
	       "  status                              Show status\n"
	       "  config                              Show running config\n");
	for (const struct help_entry *e = help_entries; e->text; e++) {
		if (e->daemon && strcmp(e->daemon, name) == 0)
			printf("  %s\n", e->text);
	}
}

static void usage(FILE *out)
{
	fprintf(out, "Usage: raptorctl <command>\n\nCommands:\n");
	const char *cur = NULL;
	for (const struct help_entry *e = help_entries; e->text; e++) {
		if (!same_section(e->daemon, cur)) {
			if (e->daemon)
				fprintf(out, "\n%s commands:\n", e->daemon);
			else if (cur)
				fprintf(out, "\n");
			cur = e->daemon;
		}
		if (e->daemon)
			fprintf(out, "  %s %s\n", e->daemon, e->text);
		else
			fprintf(out, "  %s\n", e->text);
	}
	fprintf(out,
		"\nJSON mode:\n"
		"  -j '{\"daemon\":\"rvd\",\"cmd\":\"...\"}'\n"
		"  -j "
		"'[{\"daemon\":\"rvd\",\"cmd\":\"...\"},{\"daemon\":\"rad\",\"cmd\":\"...\"}]'\n");
	fprintf(out, "\nDaemons: rvd, rsd, rad, rod, rhd, ric, rmr, rmd, rwd, rwc\n");
}

static int is_daemon(const char *name)
{
	for (int i = 0; daemons[i]; i++) {
		if (strcmp(name, daemons[i]) == 0)
			return 1;
	}
	return 0;
}

static int send_cmd(const char *daemon, const char *json)
{
	char sock_path[64];
	snprintf(sock_path, sizeof(sock_path), "/var/run/rss/%s.sock", daemon);

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

static int send_cmd_json(const char *daemon, const char *json, char *resp, int resp_size)
{
	char sock_path[64];
	snprintf(sock_path, sizeof(sock_path), "/var/run/rss/%s.sock", daemon);

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

static int handle_json_mode(const char *input)
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

int main(int argc, char **argv)
{
	if (argc < 2) {
		usage(stderr);
		return 1;
	}

	if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
		usage(stdout);
		return 0;
	}

	if (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0) {
		fprintf(stderr, "Raptor Streaming System — raptorctl [%s] built %s\n",
			rss_build_hash, rss_build_time);
		return 0;
	}

	if (strcmp(argv[1], "-j") == 0) {
		if (argc < 3) {
			fprintf(stderr, "Usage: raptorctl -j '<json>'\n");
			return 1;
		}
		return handle_json_mode(argv[2]);
	}

	if (strcmp(argv[1], "status") == 0) {
		cmd_status();
		return 0;
	}

	if (strcmp(argv[1], "memory") == 0) {
		cmd_memory();
		return 0;
	}

	if (strcmp(argv[1], "cpu") == 0) {
		cmd_cpu();
		return 0;
	}

	/* Section-to-daemon mapping is in find_daemon_for_section() */

	/* raptorctl config <subcommand> */
	if (strcmp(argv[1], "config") == 0) {
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
				snprintf(sock_path, sizeof(sock_path), "/var/run/rss/%s.sock",
					 daemons[i]);
				int ret = rss_ctrl_send_command(sock_path,
								"{\"cmd\":\"config-save\"}", resp,
								sizeof(resp), 2000);
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
					snprintf(sock_path, sizeof(sock_path),
						 "/var/run/rss/%s.sock", target);
					cJSON *j = jcmd("config-get");
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
					snprintf(sock_path, sizeof(sock_path),
						 "/var/run/rss/%s.sock", target);
					cJSON *j = jcmd("config-get-section");
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
				/* Single key */
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
				fprintf(stderr,
					"Usage: raptorctl config set <section> <key> <value>\n");
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

	/* raptorctl test-motion [duration_sec] — trigger RMR clip recording */
	if (strcmp(argv[1], "test-motion") == 0) {
		int dur = 10;
		if (argc >= 3)
			dur = (int)strtol(argv[2], NULL, 10);
		if (dur < 1)
			dur = 1;
		if (dur > 300)
			dur = 300;

		printf("triggering motion clip for %d seconds...\n", dur);
		int ret = send_cmd("rmr", "{\"cmd\":\"start\"}");
		if (ret != 0)
			return ret;
		sleep(dur);
		printf("stopping motion clip...\n");
		return send_cmd("rmr", "{\"cmd\":\"stop\"}");
	}

	if (!is_daemon(argv[1])) {
		fprintf(stderr, "Unknown daemon: %s\n", argv[1]);
		usage(stderr);
		return 1;
	}

	if (argc < 3) {
		/* raptorctl <daemon> — show status + available commands */
		int pid = rss_daemon_check(argv[1]);
		if (pid > 0)
			printf("%s: running (pid %d)\n", argv[1], pid);
		else
			printf("%s: stopped\n", argv[1]);

		daemon_help(argv[1]);
		return 0;
	}

	const char *daemon = argv[1];
	const char *cmd = argv[2];
	char json[512];

	/* Privacy is implemented by RVD but exposed under ROD for UX */
	if (strcmp(daemon, "rod") == 0 && strcmp(cmd, "privacy") == 0)
		daemon = "rvd";

	if (strcmp(cmd, "status") == 0) {
		jstr(jcmd("status"), json, sizeof(json));

	} else if (strcmp(cmd, "config") == 0) {
		jstr(jcmd("config-show"), json, sizeof(json));

	} else if (strcmp(cmd, "clients") == 0) {
		jstr(jcmd("clients"), json, sizeof(json));

	} else if (strcmp(cmd, "share-rotate") == 0) {
		jstr(jcmd("share-rotate"), json, sizeof(json));

	} else if (strcmp(cmd, "share") == 0) {
		jstr(jcmd("share"), json, sizeof(json));

	} else if (strcmp(cmd, "request-idr") == 0) {
		cJSON *j = jcmd("request-idr");
		if (argc > 3)
			jadd_i(j, "channel", argv[3]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "set-bitrate") == 0) {
		if (argc < 5) {
			fprintf(stderr, "Usage: raptorctl %s set-bitrate <channel> <bps>\n",
				daemon);
			return 1;
		}
		cJSON *j = jcmd("set-bitrate");
		jadd_i(j, "channel", argv[3]);
		jadd_i(j, "value", argv[4]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "set-gop") == 0) {
		if (argc < 5) {
			fprintf(stderr, "Usage: raptorctl %s set-gop <channel> <length>\n", daemon);
			return 1;
		}
		cJSON *j = jcmd("set-gop");
		jadd_i(j, "channel", argv[3]);
		jadd_i(j, "value", argv[4]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "set-fps") == 0) {
		if (argc < 5) {
			fprintf(stderr, "Usage: raptorctl %s set-fps <channel> <fps>\n", daemon);
			return 1;
		}
		cJSON *j = jcmd("set-fps");
		jadd_i(j, "channel", argv[3]);
		jadd_i(j, "value", argv[4]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "set-qp-bounds") == 0) {
		if (argc < 6) {
			fprintf(stderr, "Usage: raptorctl %s set-qp-bounds <channel> <min> <max>\n",
				daemon);
			return 1;
		}
		cJSON *j = jcmd("set-qp-bounds");
		jadd_i(j, "channel", argv[3]);
		jadd_i(j, "min", argv[4]);
		jadd_i(j, "max", argv[5]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "set-qp-bounds-per-frame") == 0) {
		if (argc < 8) {
			fprintf(stderr,
				"Usage: raptorctl %s set-qp-bounds-per-frame <ch> "
				"<min_i> <max_i> <min_p> <max_p>\n",
				daemon);
			return 1;
		}
		cJSON *j = jcmd("set-qp-bounds-per-frame");
		jadd_i(j, "channel", argv[3]);
		jadd_i(j, "min_i", argv[4]);
		jadd_i(j, "max_i", argv[5]);
		jadd_i(j, "min_p", argv[6]);
		jadd_i(j, "max_p", argv[7]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "set-max-pic-size") == 0) {
		if (argc < 6) {
			fprintf(stderr,
				"Usage: raptorctl %s set-max-pic-size <ch> <i_kbits> <p_kbits>\n",
				daemon);
			return 1;
		}
		cJSON *j = jcmd("set-max-pic-size");
		jadd_i(j, "channel", argv[3]);
		jadd_i(j, "i_kbits", argv[4]);
		jadd_i(j, "p_kbits", argv[5]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "set-h265-trans") == 0) {
		if (argc < 6) {
			fprintf(stderr,
				"Usage: raptorctl %s set-h265-trans <ch> <cr_offset> <cb_offset>\n",
				daemon);
			return 1;
		}
		cJSON *j = jcmd("set-h265-trans");
		jadd_i(j, "channel", argv[3]);
		jadd_i(j, "cr_offset", argv[4]);
		jadd_i(j, "cb_offset", argv[5]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "set-roi") == 0) {
		if (argc < 11) {
			fprintf(stderr,
				"Usage: raptorctl %s set-roi <ch> <idx> <enable> "
				"<x> <y> <w> <h> <qp>\n",
				daemon);
			return 1;
		}
		cJSON *j = jcmd("set-roi");
		jadd_i(j, "channel", argv[3]);
		jadd_i(j, "index", argv[4]);
		jadd_i(j, "enable", argv[5]);
		jadd_i(j, "x", argv[6]);
		jadd_i(j, "y", argv[7]);
		jadd_i(j, "w", argv[8]);
		jadd_i(j, "h", argv[9]);
		jadd_i(j, "qp", argv[10]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "get-roi") == 0) {
		if (argc < 5) {
			fprintf(stderr, "Usage: raptorctl %s get-roi <ch> <index>\n", daemon);
			return 1;
		}
		cJSON *j = jcmd("get-roi");
		jadd_i(j, "channel", argv[3]);
		jadd_i(j, "index", argv[4]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "set-super-frame") == 0) {
		if (argc < 7) {
			fprintf(stderr,
				"Usage: raptorctl %s set-super-frame <ch> <mode> "
				"<i_thr> <p_thr>\n"
				"  modes: 0=none 1=discard 2=reencode\n",
				daemon);
			return 1;
		}
		cJSON *j = jcmd("set-super-frame");
		jadd_i(j, "channel", argv[3]);
		jadd_i(j, "mode", argv[4]);
		jadd_i(j, "i_thr", argv[5]);
		jadd_i(j, "p_thr", argv[6]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "set-pskip") == 0) {
		if (argc < 6) {
			fprintf(stderr,
				"Usage: raptorctl %s set-pskip <ch> <enable> <max_frames>\n",
				daemon);
			return 1;
		}
		cJSON *j = jcmd("set-pskip");
		jadd_i(j, "channel", argv[3]);
		jadd_i(j, "enable", argv[4]);
		jadd_i(j, "max_frames", argv[5]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "set-srd") == 0) {
		if (argc < 6) {
			fprintf(stderr, "Usage: raptorctl %s set-srd <ch> <enable> <level>\n",
				daemon);
			return 1;
		}
		cJSON *j = jcmd("set-srd");
		jadd_i(j, "channel", argv[3]);
		jadd_i(j, "enable", argv[4]);
		jadd_i(j, "level", argv[5]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "set-enc-denoise") == 0) {
		if (argc < 8) {
			fprintf(stderr,
				"Usage: raptorctl %s set-enc-denoise <ch> <enable> "
				"<type> <i_qp> <p_qp>\n"
				"  types: 0=off 1=I+P 2=I-only\n",
				daemon);
			return 1;
		}
		cJSON *j = jcmd("set-enc-denoise");
		jadd_i(j, "channel", argv[3]);
		jadd_i(j, "enable", argv[4]);
		jadd_i(j, "type", argv[5]);
		jadd_i(j, "i_qp", argv[6]);
		jadd_i(j, "p_qp", argv[7]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "set-gdr") == 0) {
		if (argc < 6) {
			fprintf(stderr, "Usage: raptorctl %s set-gdr <ch> <enable> <cycle>\n",
				daemon);
			return 1;
		}
		cJSON *j = jcmd("set-gdr");
		jadd_i(j, "channel", argv[3]);
		jadd_i(j, "enable", argv[4]);
		jadd_i(j, "cycle", argv[5]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "request-pskip") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl %s request-pskip <channel>\n", daemon);
			return 1;
		}
		cJSON *j = jcmd("request-pskip");
		jadd_i(j, "channel", argv[3]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "request-gdr") == 0) {
		if (argc < 5) {
			fprintf(stderr, "Usage: raptorctl %s request-gdr <channel> <frames>\n",
				daemon);
			return 1;
		}
		cJSON *j = jcmd("request-gdr");
		jadd_i(j, "channel", argv[3]);
		jadd_i(j, "value", argv[4]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "set-enc-crop") == 0) {
		if (argc < 9) {
			fprintf(stderr,
				"Usage: raptorctl %s set-enc-crop <ch> <enable> "
				"<x> <y> <w> <h>\n",
				daemon);
			return 1;
		}
		cJSON *j = jcmd("set-enc-crop");
		jadd_i(j, "channel", argv[3]);
		jadd_i(j, "enable", argv[4]);
		jadd_i(j, "x", argv[5]);
		jadd_i(j, "y", argv[6]);
		jadd_i(j, "w", argv[7]);
		jadd_i(j, "h", argv[8]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "set-qp") == 0 || strcmp(cmd, "set-qp-ip-delta") == 0 ||
		   strcmp(cmd, "set-gop-mode") == 0 || strcmp(cmd, "set-rc-options") == 0 ||
		   strcmp(cmd, "set-max-same-scene") == 0 || strcmp(cmd, "set-qpg-mode") == 0 ||
		   strcmp(cmd, "set-entropy-mode") == 0 ||
		   strcmp(cmd, "set-stream-buf-size") == 0 || strcmp(cmd, "set-jpeg-qp") == 0 ||
		   strcmp(cmd, "set-color2grey") == 0 || strcmp(cmd, "set-mbrc") == 0 ||
		   strcmp(cmd, "set-resize-mode") == 0 || strcmp(cmd, "set-h264-trans") == 0) {
		/* Encoder set commands: <channel> <value> */
		if (argc < 5) {
			fprintf(stderr, "Usage: raptorctl %s %s <channel> <value>\n", daemon, cmd);
			return 1;
		}
		cJSON *j = jcmd(cmd);
		jadd_i(j, "channel", argv[3]);
		jadd_i(j, "value", argv[4]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "get-enc-caps") == 0) {
		jstr(jcmd("get-enc-caps"), json, sizeof(json));

	} else if (strcmp(cmd, "get-bitrate") == 0 || strcmp(cmd, "get-fps") == 0 ||
		   strcmp(cmd, "get-gop") == 0 || strcmp(cmd, "get-qp-bounds") == 0 ||
		   strcmp(cmd, "get-rc-mode") == 0 || strcmp(cmd, "get-gop-mode") == 0 ||
		   strcmp(cmd, "get-rc-options") == 0 || strcmp(cmd, "get-max-same-scene") == 0 ||
		   strcmp(cmd, "get-color2grey") == 0 || strcmp(cmd, "get-mbrc") == 0 ||
		   strcmp(cmd, "get-qpg-mode") == 0 || strcmp(cmd, "get-stream-buf-size") == 0 ||
		   strcmp(cmd, "get-h264-trans") == 0 || strcmp(cmd, "get-h265-trans") == 0 ||
		   strcmp(cmd, "get-super-frame") == 0 || strcmp(cmd, "get-pskip") == 0 ||
		   strcmp(cmd, "get-srd") == 0 || strcmp(cmd, "get-enc-denoise") == 0 ||
		   strcmp(cmd, "get-gdr") == 0 || strcmp(cmd, "get-enc-crop") == 0 ||
		   strcmp(cmd, "get-jpeg-qp") == 0) {
		/* Encoder get commands: <channel> */
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl %s %s <channel>\n", daemon, cmd);
			return 1;
		}
		cJSON *j = jcmd(cmd);
		jadd_i(j, "channel", argv[3]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "set-volume") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl %s set-volume <value>\n", daemon);
			return 1;
		}
		cJSON *j = jcmd("set-volume");
		jadd_i(j, "value", argv[3]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "set-gain") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl %s set-gain <value>\n", daemon);
			return 1;
		}
		cJSON *j = jcmd("set-gain");
		jadd_i(j, "value", argv[3]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "ao-set-volume") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl %s ao-set-volume <value>\n", daemon);
			return 1;
		}
		cJSON *j = jcmd("ao-set-volume");
		jadd_i(j, "value", argv[3]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "ao-set-gain") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl %s ao-set-gain <value>\n", daemon);
			return 1;
		}
		cJSON *j = jcmd("ao-set-gain");
		jadd_i(j, "value", argv[3]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "set-ns") == 0) {
		if (argc < 4) {
			fprintf(stderr,
				"Usage: raptorctl %s set-ns <0|1> [0-3]\n"
				"  levels: 0=low 1=moderate 2=high 3=veryhigh\n",
				daemon);
			return 1;
		}
		cJSON *j = jcmd("set-ns");
		jadd_i(j, "value", argv[3]);
		if (argc >= 5)
			jadd_i(j, "level", argv[4]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "set-hpf") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl %s set-hpf <0|1>\n", daemon);
			return 1;
		}
		cJSON *j = jcmd("set-hpf");
		jadd_i(j, "value", argv[3]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "set-agc") == 0) {
		if (argc < 4) {
			fprintf(stderr,
				"Usage: raptorctl %s set-agc <0|1> [target] [compression]\n",
				daemon);
			return 1;
		}
		cJSON *j = jcmd("set-agc");
		jadd_i(j, "value", argv[3]);
		if (argc >= 5)
			jadd_i(j, "target", argv[4]);
		if (argc >= 6)
			jadd_i(j, "compression", argv[5]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "privacy") == 0) {
		cJSON *j = jcmd("privacy");
		if (argc > 3)
			jadd_s(j, "value", argv[3]);
		if (argc > 4)
			jadd_i(j, "channel", argv[4]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "stream-stop") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl %s stream-stop <channel>\n", daemon);
			return 1;
		}
		cJSON *j = jcmd("stream-stop");
		jadd_i(j, "channel", argv[3]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "stream-start") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl %s stream-start <channel>\n", daemon);
			return 1;
		}
		cJSON *j = jcmd("stream-start");
		jadd_i(j, "channel", argv[3]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "stream-restart") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl %s stream-restart <channel>\n", daemon);
			return 1;
		}
		cJSON *j = jcmd("stream-restart");
		jadd_i(j, "channel", argv[3]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "set-codec") == 0) {
		if (strcmp(daemon, "rad") == 0) {
			/* RAD: set-codec <codec> (no channel) */
			if (argc < 4) {
				fprintf(stderr, "Usage: raptorctl rad set-codec "
						"<pcmu|pcma|l16|aac|opus>\n");
				return 1;
			}
			cJSON *j = jcmd("set-codec");
			jadd_s(j, "value", argv[3]);
			jstr(j, json, sizeof(json));
		} else {
			/* RVD: set-codec <channel> <h264|h265> */
			if (argc < 5) {
				fprintf(stderr,
					"Usage: raptorctl %s set-codec <channel> <h264|h265>\n",
					daemon);
				return 1;
			}
			cJSON *j = jcmd("set-codec");
			jadd_i(j, "channel", argv[3]);
			jadd_s(j, "value", argv[4]);
			jstr(j, json, sizeof(json));
		}

	} else if (strcmp(cmd, "set-resolution") == 0) {
		if (argc < 6) {
			fprintf(stderr,
				"Usage: raptorctl %s set-resolution <channel> <width> <height>\n",
				daemon);
			return 1;
		}
		cJSON *j = jcmd("set-resolution");
		jadd_i(j, "channel", argv[3]);
		jadd_i(j, "width", argv[4]);
		jadd_i(j, "height", argv[5]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "osd-restart") == 0) {
		cJSON *j = jcmd("osd-restart");
		if (argc > 3)
			jadd_i(j, "pool_kb", argv[3]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "mode") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl %s mode <auto|day|night>\n", daemon);
			return 1;
		}
		cJSON *j = jcmd("mode");
		jadd_s(j, "value", argv[3]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "isp-mode") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl %s isp-mode <day|night>\n", daemon);
			return 1;
		}
		cJSON *j = jcmd("isp-mode");
		jadd_s(j, "value", argv[3]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "set-rc-mode") == 0) {
		if (argc < 5) {
			fprintf(stderr,
				"Usage: raptorctl %s set-rc-mode <ch> <mode> [bitrate]\n"
				"  modes: fixqp cbr vbr smart capped_vbr capped_quality\n",
				daemon);
			return 1;
		}
		cJSON *j = jcmd("set-rc-mode");
		jadd_i(j, "channel", argv[3]);
		jadd_s(j, "mode", argv[4]);
		if (argc >= 6)
			jadd_i(j, "bitrate", argv[5]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "set-wb") == 0) {
		if (argc < 4) {
			fprintf(stderr,
				"Usage: raptorctl %s set-wb <auto|manual> [r_gain] [b_gain]\n",
				daemon);
			return 1;
		}
		cJSON *j = jcmd("set-wb");
		jadd_s(j, "mode", argv[3]);
		if (argc >= 5)
			jadd_i(j, "r_gain", argv[4]);
		if (argc >= 6)
			jadd_i(j, "b_gain", argv[5]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "set-font-color") == 0 || strcmp(cmd, "set-stroke-color") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl %s %s <0xAARRGGBB>\n", daemon, cmd);
			return 1;
		}
		cJSON *j = jcmd(cmd);
		jadd_s(j, "value", argv[3]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "set-position") == 0) {
		if (argc < 5) {
			fprintf(stderr,
				"Usage: raptorctl %s set-position <element> <pos>\n"
				"  element: time, uptime, text, logo\n"
				"  pos: top_left, top_center, top_right, bottom_left,\n"
				"       bottom_center, bottom_right, center, or x,y\n",
				daemon);
			return 1;
		}
		cJSON *j = jcmd("set-position");
		jadd_s(j, "element", argv[3]);
		jadd_s(j, "pos", argv[4]);
		if (argc > 5)
			jadd_i(j, "stream", argv[5]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "set-font-size") == 0 || strcmp(cmd, "set-stroke-size") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl %s %s <value>\n", daemon, cmd);
			return 1;
		}
		cJSON *j = jcmd(cmd);
		jadd_i(j, "value", argv[3]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "add-element") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl %s add-element <name> [key=val]...\n",
				daemon);
			return 1;
		}
		cJSON *j = jcmd("add-element");
		jadd_s(j, "name", argv[3]);
		for (int a = 4; a < argc; a++) {
			char *eq = strchr(argv[a], '=');
			if (!eq)
				continue;
			*eq = '\0';
			jadd_auto(j, argv[a], eq + 1);
			*eq = '=';
		}
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "remove-element") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl %s remove-element <name>\n", daemon);
			return 1;
		}
		cJSON *j = jcmd("remove-element");
		jadd_s(j, "name", argv[3]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "set-element") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl %s set-element <name> [key=val]...\n",
				daemon);
			return 1;
		}
		cJSON *j = jcmd("set-element");
		jadd_s(j, "name", argv[3]);
		for (int a = 4; a < argc; a++) {
			char *eq = strchr(argv[a], '=');
			if (!eq)
				continue;
			*eq = '\0';
			jadd_auto(j, argv[a], eq + 1);
			*eq = '=';
		}
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "show-element") == 0 || strcmp(cmd, "hide-element") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl %s %s <name>\n", daemon, cmd);
			return 1;
		}
		cJSON *j = jcmd(cmd);
		jadd_s(j, "name", argv[3]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "set-var") == 0) {
		if (argc < 5) {
			fprintf(stderr, "Usage: raptorctl %s set-var <name> <value>\n", daemon);
			return 1;
		}
		cJSON *j = jcmd("set-var");
		jadd_s(j, "name", argv[3]);
		jadd_s(j, "value", argv[4]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "sensitivity") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl %s sensitivity <0-4>\n", daemon);
			return 1;
		}
		cJSON *j = jcmd("sensitivity");
		jadd_i(j, "value", argv[3]);
		jstr(j, json, sizeof(json));

	} else if (strcmp(cmd, "skip-frames") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl %s skip-frames <N>\n", daemon);
			return 1;
		}
		cJSON *j = jcmd("skip-frames");
		jadd_i(j, "value", argv[3]);
		jstr(j, json, sizeof(json));

	} else if (strncmp(cmd, "set-", 4) == 0 && argc >= 4) {
		/* Generic set-X <value> pass-through.
		 * Optional --sensor N flag for multi-sensor ISP tuning. */
		int sensor_idx = -1;
		const char *val_arg = argv[3];
		if (argc >= 6 && strcmp(argv[4], "--sensor") == 0)
			sensor_idx = (int)strtol(argv[5], NULL, 10);
		else if (argc >= 5 && strcmp(argv[3], "--sensor") == 0) {
			sensor_idx = (int)strtol(argv[4], NULL, 10);
			val_arg = argc >= 6 ? argv[5] : "0";
		}
		cJSON *j = jcmd(cmd);
		jadd_i(j, "value", val_arg);
		if (sensor_idx >= 0)
			cJSON_AddNumberToObject(j, "sensor", sensor_idx);
		jstr(j, json, sizeof(json));

	} else if (strncmp(cmd, "get-", 4) == 0) {
		/* Generic get-X pass-through */
		jstr(jcmd(cmd), json, sizeof(json));

	} else {
		/* Generic pass-through */
		jstr(jcmd(cmd), json, sizeof(json));
	}

	return send_cmd(daemon, json);
}

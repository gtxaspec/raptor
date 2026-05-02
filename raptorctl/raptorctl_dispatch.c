/*
 * raptorctl_dispatch.c -- Table-driven command dispatch
 *
 * Maps CLI commands to JSON IPC payloads via a static dispatch table.
 * Commands with regular arg layouts (positional ints/strings) use the
 * table. Commands with varargs or daemon-dependent behavior get small
 * special-case handlers.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cJSON.h>

#include "raptorctl.h"

/* ------------------------------------------------------------------ */
/* Arg type enum and descriptor                                       */
/* ------------------------------------------------------------------ */

enum arg_type { A_END = 0, A_INT, A_STR };

struct cmd_arg {
	const char *key;
	enum arg_type type;
};

struct cmd_def {
	const char *name;
	const char *json_cmd; /* NULL = same as name */
	int min_args;
	const struct cmd_arg *args; /* A_END-terminated, entries beyond min_args are optional */
};

/* ------------------------------------------------------------------ */
/* Shared arg layouts — commands with the same signature share these   */
/* ------------------------------------------------------------------ */

static const struct cmd_arg args_none[] = {{NULL, A_END}};

static const struct cmd_arg args_ch[] = {{"channel", A_INT}, {NULL, A_END}};

static const struct cmd_arg args_ch_val[] = {{"channel", A_INT}, {"value", A_INT}, {NULL, A_END}};

static const struct cmd_arg args_val[] = {{"value", A_INT}, {NULL, A_END}};

static const struct cmd_arg args_val_str[] = {{"value", A_STR}, {NULL, A_END}};

static const struct cmd_arg args_name[] = {{"name", A_STR}, {NULL, A_END}};

static const struct cmd_arg args_name_val_str[] = {
	{"name", A_STR}, {"value", A_STR}, {NULL, A_END}};

/* ------------------------------------------------------------------ */
/* Unique arg layouts for multi-arg commands                          */
/* ------------------------------------------------------------------ */

static const struct cmd_arg args_ch_min_max[] = {
	{"channel", A_INT}, {"min", A_INT}, {"max", A_INT}, {NULL, A_END}};

static const struct cmd_arg args_qp_per_frame[] = {{"channel", A_INT}, {"min_i", A_INT},
						   {"max_i", A_INT},   {"min_p", A_INT},
						   {"max_p", A_INT},   {NULL, A_END}};

static const struct cmd_arg args_max_pic_size[] = {
	{"channel", A_INT}, {"i_kbits", A_INT}, {"p_kbits", A_INT}, {NULL, A_END}};

static const struct cmd_arg args_h265_trans[] = {
	{"channel", A_INT}, {"cr_offset", A_INT}, {"cb_offset", A_INT}, {NULL, A_END}};

static const struct cmd_arg args_roi[] = {{"channel", A_INT}, {"index", A_INT}, {"enable", A_INT},
					  {"x", A_INT},	      {"y", A_INT},	{"w", A_INT},
					  {"h", A_INT},	      {"qp", A_INT},	{NULL, A_END}};

static const struct cmd_arg args_ch_idx[] = {{"channel", A_INT}, {"index", A_INT}, {NULL, A_END}};

static const struct cmd_arg args_super_frame[] = {
	{"channel", A_INT}, {"mode", A_INT}, {"i_thr", A_INT}, {"p_thr", A_INT}, {NULL, A_END}};

static const struct cmd_arg args_pskip[] = {
	{"channel", A_INT}, {"enable", A_INT}, {"max_frames", A_INT}, {NULL, A_END}};

static const struct cmd_arg args_srd[] = {
	{"channel", A_INT}, {"enable", A_INT}, {"level", A_INT}, {NULL, A_END}};

static const struct cmd_arg args_enc_denoise[] = {{"channel", A_INT}, {"enable", A_INT},
						  {"type", A_INT},    {"i_qp", A_INT},
						  {"p_qp", A_INT},    {NULL, A_END}};

static const struct cmd_arg args_gdr[] = {
	{"channel", A_INT}, {"enable", A_INT}, {"cycle", A_INT}, {NULL, A_END}};

static const struct cmd_arg args_enc_crop[] = {{"channel", A_INT}, {"enable", A_INT}, {"x", A_INT},
					       {"y", A_INT},	   {"w", A_INT},      {"h", A_INT},
					       {NULL, A_END}};

static const struct cmd_arg args_resolution[] = {
	{"channel", A_INT}, {"width", A_INT}, {"height", A_INT}, {NULL, A_END}};

static const struct cmd_arg args_ns[] = {{"value", A_INT}, {"level", A_INT}, {NULL, A_END}};

static const struct cmd_arg args_agc[] = {
	{"value", A_INT}, {"target", A_INT}, {"compression", A_INT}, {NULL, A_END}};

static const struct cmd_arg args_privacy[] = {{"value", A_STR}, {"channel", A_INT}, {NULL, A_END}};

static const struct cmd_arg args_rc_mode[] = {
	{"channel", A_INT}, {"mode", A_STR}, {"bitrate", A_INT}, {NULL, A_END}};

static const struct cmd_arg args_wb[] = {
	{"mode", A_STR}, {"r_gain", A_INT}, {"b_gain", A_INT}, {NULL, A_END}};

static const struct cmd_arg args_position[] = {
	{"element", A_STR}, {"pos", A_STR}, {"stream", A_INT}, {NULL, A_END}};

static const struct cmd_arg args_pool_kb[] = {{"pool_kb", A_INT}, {NULL, A_END}};

static const struct cmd_arg args_cpu[] = {{"cpu", A_INT}, {NULL, A_END}};

static const struct cmd_arg args_key_val[] = {{"key", A_STR}, {"value", A_INT}, {NULL, A_END}};

static const struct cmd_arg args_enc_set[] = {
	{"channel", A_INT}, {"param", A_STR}, {"value", A_INT}, {NULL, A_END}};

static const struct cmd_arg args_enc_get[] = {{"channel", A_INT}, {"param", A_STR}, {NULL, A_END}};

/* ------------------------------------------------------------------ */
/* Dispatch table                                                     */
/*                                                                    */
/* min_args = minimum positional args after the command name.          */
/* Args beyond min_args up to the A_END sentinel are optional.        */
/* json_cmd overrides the JSON "cmd" field when non-NULL.             */
/* ------------------------------------------------------------------ */

static const struct cmd_def cmd_table[] = {
	/* No-arg daemon commands */
	{"status", NULL, 0, args_none},
	{"config", "config-show", 0, args_none},
	{"clients", NULL, 0, args_none},
	{"share", NULL, 0, args_none},
	{"share-rotate", NULL, 0, args_none},
	{"get-enc-caps", NULL, 0, args_none},

	/* Optional channel */
	{"request-idr", NULL, 0, args_ch},

	/* Video encoder: channel + value (config-persisting) */
	{"set-bitrate", NULL, 2, args_ch_val},
	{"set-gop", NULL, 2, args_ch_val},
	{"set-fps", NULL, 2, args_ch_val},
	{"set-h264-trans", NULL, 2, args_ch_val},

	/* Video encoder: multi-arg set commands */
	{"set-qp-bounds", NULL, 3, args_ch_min_max},
	{"set-qp-bounds-per-frame", NULL, 5, args_qp_per_frame},
	{"set-max-pic-size", NULL, 3, args_max_pic_size},
	{"set-h265-trans", NULL, 3, args_h265_trans},
	{"set-roi", NULL, 8, args_roi},
	{"set-super-frame", NULL, 4, args_super_frame},
	{"set-pskip", NULL, 3, args_pskip},
	{"set-srd", NULL, 3, args_srd},
	{"set-enc-denoise", NULL, 5, args_enc_denoise},
	{"set-gdr", NULL, 3, args_gdr},
	{"set-enc-crop", NULL, 6, args_enc_crop},

	/* Video encoder: channel-only get commands (struct/custom response) */
	{"get-bitrate", NULL, 1, args_ch},
	{"get-fps", NULL, 1, args_ch},
	{"get-gop", NULL, 1, args_ch},
	{"get-qp-bounds", NULL, 1, args_ch},
	{"get-rc-mode", NULL, 1, args_ch},
	{"get-h264-trans", NULL, 1, args_ch},
	{"get-h265-trans", NULL, 1, args_ch},
	{"get-super-frame", NULL, 1, args_ch},
	{"get-pskip", NULL, 1, args_ch},
	{"get-srd", NULL, 1, args_ch},
	{"get-enc-denoise", NULL, 1, args_ch},
	{"get-gdr", NULL, 1, args_ch},
	{"get-enc-crop", NULL, 1, args_ch},
	{"get-roi", NULL, 2, args_ch_idx},
	{"request-pskip", NULL, 1, args_ch},
	{"request-gdr", NULL, 2, args_ch_val},

	/* Table-driven encoder params (enc-list handled by special handler) */
	{"enc-set", NULL, 3, args_enc_set},
	{"enc-get", NULL, 2, args_enc_get},

	/* Stream control */
	{"stream-stop", NULL, 1, args_ch},
	{"stream-start", NULL, 1, args_ch},
	{"stream-restart", NULL, 1, args_ch},
	{"set-resolution", NULL, 3, args_resolution},
	{"set-jpeg-quality", NULL, 2, args_ch_val},
	{"set-rc-mode", NULL, 2, args_rc_mode},

	/* Audio */
	{"set-aec", NULL, 1, args_val},
	{"set-volume", NULL, 1, args_val},
	{"set-gain", NULL, 1, args_val},
	{"set-sample-rate", NULL, 1, args_val},
	{"ao-set-volume", NULL, 1, args_val},
	{"ao-set-gain", NULL, 1, args_val},
	{"ao-set-sample-rate", NULL, 1, args_val},
	{"set-hpf", NULL, 1, args_val},
	{"set-ns", NULL, 1, args_ns},
	{"set-agc", NULL, 1, args_agc},

	/* Privacy (rod redirects to rvd in main) */
	{"privacy", NULL, 0, args_privacy},

	/* IRcut */
	{"mode", NULL, 1, args_val_str},
	{"isp-mode", NULL, 1, args_val_str},
	{"set-threshold", NULL, 2, args_key_val},
	{"get-thresholds", NULL, 0, args_none},

	/* OSD */
	{"osd-restart", NULL, 0, args_pool_kb},
	{"set-font-color", NULL, 1, args_val_str},
	{"set-stroke-color", NULL, 1, args_val_str},
	{"set-font-size", NULL, 1, args_val},
	{"set-stroke-size", NULL, 1, args_val},
	{"set-time-format", NULL, 1, args_val_str},
	{"set-url", NULL, 1, args_val_str},
	{"set-position", NULL, 2, args_position},
	{"remove-element", NULL, 1, args_name},
	{"show-element", NULL, 1, args_name},
	{"hide-element", NULL, 1, args_name},
	{"set-var", NULL, 2, args_name_val_str},
	{"receipt-clear", NULL, 0, args_name},

	/* Motion detection */
	{"sensitivity", NULL, 1, args_val},
	{"skip-frames", NULL, 1, args_val},

	/* Common to all daemons */
	{"set-affinity", NULL, 1, args_cpu},
	{"get-affinity", NULL, 0, args_none},
	{"set-log-level", NULL, 1, args_val_str},
	{"get-log-level", NULL, 0, args_none},

	/* White balance */
	{"set-wb", NULL, 1, args_wb},

	/* Sentinel */
	{NULL, NULL, 0, NULL},
};

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static int count_args(const struct cmd_arg *args)
{
	int n = 0;
	while (args[n].type != A_END)
		n++;
	return n;
}

static void print_cmd_usage(const char *daemon, const struct cmd_def *d)
{
	int max_args = count_args(d->args);
	fprintf(stderr, "Usage: raptorctl %s %s", daemon, d->name);
	for (int i = 0; i < max_args; i++) {
		if (i < d->min_args)
			fprintf(stderr, " <%s>", d->args[i].key);
		else
			fprintf(stderr, " [%s]", d->args[i].key);
	}
	fprintf(stderr, "\n");
}

/* ------------------------------------------------------------------ */
/* Special handlers for commands that don't fit the table             */
/* ------------------------------------------------------------------ */

static int build_set_codec(const char *daemon, int argc, char **argv, char *json, int json_size)
{
	if (strcmp(daemon, "rad") == 0) {
		if (argc < 4) {
			fprintf(stderr,
				"Usage: raptorctl rad set-codec <pcmu|pcma|l16|aac|opus>\n");
			return -1;
		}
		cJSON *j = jcmd("set-codec");
		if (!j)
			return -1;
		jadd_s(j, "value", argv[3]);
		jstr(j, json, json_size);
	} else {
		if (argc < 5) {
			fprintf(stderr, "Usage: raptorctl %s set-codec <channel> <h264|h265>\n",
				daemon);
			return -1;
		}
		cJSON *j = jcmd("set-codec");
		if (!j)
			return -1;
		jadd_i(j, "channel", argv[3]);
		jadd_s(j, "value", argv[4]);
		jstr(j, json, json_size);
	}
	return 1;
}

static int build_element_cmd(const char *cmd, int argc, char **argv, char *json, int json_size)
{
	if (argc < 4) {
		fprintf(stderr, "Usage: raptorctl <daemon> %s <name> [key=val]...\n", cmd);
		return -1;
	}
	cJSON *j = jcmd(cmd);
	if (!j)
		return -1;
	jadd_s(j, "name", argv[3]);
	for (int a = 4; a < argc; a++) {
		char *eq = strchr(argv[a], '=');
		if (!eq)
			continue;
		*eq = '\0';
		jadd_auto(j, argv[a], eq + 1);
		*eq = '=';
	}
	jstr(j, json, json_size);
	return 1;
}

static int build_receipt(const char *daemon, int argc, char **argv, char *json, int json_size)
{
	if (argc < 4) {
		fprintf(stderr, "Usage: raptorctl %s receipt [name] <text>\n", daemon);
		return -1;
	}
	cJSON *j = jcmd("receipt");
	if (!j)
		return -1;
	if (argc >= 5) {
		jadd_s(j, "name", argv[3]);
		jadd_s(j, "text", argv[4]);
	} else {
		jadd_s(j, "text", argv[3]);
	}
	jstr(j, json, json_size);
	return 1;
}

/* ------------------------------------------------------------------ */
/* Public interface                                                   */
/* ------------------------------------------------------------------ */

static int handle_enc_list(const char *daemon, int argc, char **argv)
{
	char cmd_json[128];
	if (argc >= 4) {
		cJSON *j = jcmd("enc-list");
		if (!j)
			return 1;
		jadd_i(j, "channel", argv[3]);
		jstr(j, cmd_json, sizeof(cmd_json));
	} else {
		snprintf(cmd_json, sizeof(cmd_json), "{\"cmd\":\"enc-list\"}");
	}

	char resp[4096];
	if (send_cmd_json(daemon, cmd_json, resp, sizeof(resp)) != 0) {
		fprintf(stderr, "Failed to send to %s\n", daemon);
		return 1;
	}

	cJSON *root = cJSON_Parse(resp);
	if (!root) {
		printf("%s\n", resp);
		return 0;
	}

	cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
	if (!params || !cJSON_IsArray(params)) {
		printf("%s\n", resp);
		cJSON_Delete(root);
		return 0;
	}

	bool has_values = false;
	cJSON *scan;
	cJSON_ArrayForEach(scan, params)
	{
		if (cJSON_GetObjectItem(scan, "value")) {
			has_values = true;
			break;
		}
	}

	if (has_values) {
		printf("%-20s  %-5s  %-3s  %-3s  %s\n", "PARAM", "TYPE", "SET", "GET", "VALUE");
		printf("%-20s  %-5s  %-3s  %-3s  %s\n", "--------------------", "-----", "---",
		       "---", "----------");
	} else {
		printf("%-20s  %-5s  %-3s  %-3s\n", "PARAM", "TYPE", "SET", "GET");
		printf("%-20s  %-5s  %-3s  %-3s\n", "--------------------", "-----", "---", "---");
	}

	cJSON *p;
	cJSON_ArrayForEach(p, params)
	{
		const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(p, "name"));
		const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(p, "type"));
		cJSON *set_obj = cJSON_GetObjectItem(p, "set");
		cJSON *get_obj = cJSON_GetObjectItem(p, "get");
		bool has_set = set_obj && cJSON_IsTrue(set_obj);
		bool has_get = get_obj && cJSON_IsTrue(get_obj);

		if (!name)
			continue;

		printf("%-20s  %-5s  %-3s  %-3s", name, type ? type : "?", has_set ? "yes" : "-",
		       has_get ? "yes" : "-");

		if (has_values) {
			cJSON *val = cJSON_GetObjectItem(p, "value");
			if (val) {
				if (cJSON_IsBool(val))
					printf("  %s", cJSON_IsTrue(val) ? "true" : "false");
				else if (cJSON_IsNumber(val))
					printf("  %.0f", cJSON_GetNumberValue(val));
			}
		}
		printf("\n");
	}

	cJSON_Delete(root);
	return 0;
}

/*
 * dispatch_daemon_cmd — build JSON for a daemon command.
 *
 * Returns:
 *   2  — fully handled (sent + printed), caller should not send
 *   1  — command found, json buffer filled
 *   0  — command not in table (caller should try generic fallback)
 *  -1  — command found but arg count wrong (usage printed to stderr)
 */
int dispatch_daemon_cmd(const char *daemon, const char *cmd, int argc, char **argv, char *json,
			int json_size)
{
	/* Special handlers */
	if (strcmp(cmd, "enc-list") == 0)
		return handle_enc_list(daemon, argc, argv) == 0 ? 2 : -1;
	if (strcmp(cmd, "set-codec") == 0)
		return build_set_codec(daemon, argc, argv, json, json_size);
	if (strcmp(cmd, "add-element") == 0 || strcmp(cmd, "set-element") == 0)
		return build_element_cmd(cmd, argc, argv, json, json_size);
	if (strcmp(cmd, "receipt") == 0)
		return build_receipt(daemon, argc, argv, json, json_size);

	/* Table lookup */
	for (const struct cmd_def *d = cmd_table; d->name; d++) {
		if (strcmp(cmd, d->name) != 0)
			continue;

		int extra = argc - 3;
		if (extra < d->min_args) {
			print_cmd_usage(daemon, d);
			return -1;
		}

		int max_args = count_args(d->args);
		int nargs = extra < max_args ? extra : max_args;

		const char *jc = d->json_cmd ? d->json_cmd : d->name;
		cJSON *j = jcmd(jc);
		if (!j)
			return -1;
		for (int i = 0; i < nargs; i++) {
			if (d->args[i].type == A_INT)
				jadd_i(j, d->args[i].key, argv[3 + i]);
			else
				jadd_s(j, d->args[i].key, argv[3 + i]);
		}
		jstr(j, json, json_size);
		return 1;
	}

	return 0;
}

/*
 * build_generic_set — fallback for set-* commands not in the table.
 * Handles ISP tuning commands with optional --sensor flag.
 */
int build_generic_set(const char *cmd, int argc, char **argv, char *json, int json_size)
{
	int sensor_idx = -1;
	const char *val_arg = argv[3];

	if (argc >= 6 && strcmp(argv[4], "--sensor") == 0) {
		sensor_idx = (int)strtol(argv[5], NULL, 10);
	} else if (argc >= 5 && strcmp(argv[3], "--sensor") == 0) {
		sensor_idx = (int)strtol(argv[4], NULL, 10);
		val_arg = argc >= 6 ? argv[5] : "0";
	}

	cJSON *j = jcmd(cmd);
	if (!j)
		return -1;
	jadd_i(j, "value", val_arg);
	if (sensor_idx >= 0)
		cJSON_AddNumberToObject(j, "sensor", (double)sensor_idx);
	jstr(j, json, json_size);
	return 0;
}

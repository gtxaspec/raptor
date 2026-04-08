/*
 * raptorctl.c -- Raptor control CLI
 *
 * Usage:
 *   raptorctl status                       Check which daemons are running
 *   raptorctl get <section> <key>          Read a config value
 *   raptorctl set <section> <key> <value>  Set a config value (writes to file)
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
 *   raptorctl rod status                   Show OSD region status
 *   raptorctl rod config                   Show running config
 *   raptorctl rod set-text <text>          Change OSD text string
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <rss_ipc.h>
#include <rss_common.h>

#include "raptorctl.h"

const char *daemons[] = {"rvd", "rsd", "rad", "rod", "rhd", "ric",
			 "rmr", "rmd", "rwd", "rwc", NULL};

const struct help_entry help_entries[] = {
	{NULL, "status                              Show daemon status"},
	{NULL, "memory                              Show memory usage (private/shared)"},
	{NULL, "cpu                                 Show CPU usage (1s sample)"},
	{NULL, "get <section> <key>                 Read config value"},
	{NULL, "set <section> <key> <value>         Set config value"},
	{NULL, "config save                         Save running config to disk"},
	{NULL, "<daemon> status                     Show daemon details"},
	{NULL, "<daemon> config                     Show running config"},
	{NULL, "<daemon> <cmd> [args...]            Send command"},
	{"rvd", "set-rc-mode <ch> <mode> [bps]       Change rate control mode"},
	{"rvd", "set-bitrate <ch> <bps>              Change bitrate"},
	{"rvd", "set-gop <ch> <length>               Change GOP length"},
	{"rvd", "set-fps <ch> <fps>                  Change frame rate"},
	{"rvd", "set-qp-bounds <ch> <min> <max>      Change QP range"},
	{"rvd", "request-idr [channel]               Request keyframe"},
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
	{"rad", "set-volume <val>                    Input volume"},
	{"rad", "set-gain <val>                      Input gain"},
	{"rad", "set-alc-gain <0-7>                  ALC gain (T21/T31 only)"},
	{"rad", "set-ns <0|1> [level]                Noise suppression"},
	{"rad", "set-hpf <0|1>                       High-pass filter"},
	{"rad", "set-agc <0|1> [target] [comp]       Automatic gain control"},
	{"rad", "ao-set-volume <val>                 Speaker volume"},
	{"rad", "ao-set-gain <val>                   Speaker gain"},
	{"rod", "privacy [on|off] [channel]          Toggle privacy mode"},
	{"rod", "set-text <text>                     Change OSD text"},
	{"rod", "set-font-color <0xAARRGGBB>         Text color"},
	{"rod", "set-stroke-color <0xAARRGGBB>       Stroke color"},
	{"rod", "set-stroke-size <0-5>               Stroke width"},
	{"ric", "mode <auto|day|night>               Set day/night mode (GPIO + ISP)"},
	{"ric", "isp-mode <day|night>                Set ISP mode only (no GPIO)"},
	{"rhd", "clients                             List connected clients"},
	{"rwd", "clients                             List connected clients"},
	{"rwd", "share                               Show WebTorrent share URL"},
	{"rwd", "share-rotate                        Generate new share key"},
	{"rmd", "sensitivity <0-4>                   Set motion sensitivity"},
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

	/* Map config section to daemon that owns it */
	static const struct {
		const char *section;
		const char *daemon;
	} section_map[] = {
		{"sensor", "rvd"}, {"stream0", "rvd"}, {"stream1", "rvd"},   {"jpeg", "rvd"},
		{"ring", "rvd"},   {"audio", "rad"},   {"rtsp", "rsd"},	     {"http", "rhd"},
		{"osd", "rod"},	   {"ircut", "ric"},   {"recording", "rmr"}, {"motion", "rmd"},
		{"log", "rvd"},	   {NULL, NULL},
	};

	/* raptorctl get <section> <key> — query live value from daemon, fall back to file */
	if (strcmp(argv[1], "get") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl get <section> <key>\n");
			return 1;
		}
		const char *section = argv[2];
		const char *key = argv[3];

		/* Try daemon first */
		const char *target = NULL;
		for (int i = 0; section_map[i].section; i++) {
			if (strcmp(section, section_map[i].section) == 0) {
				target = section_map[i].daemon;
				break;
			}
		}
		if (target) {
			char sock_path[64];
			char resp[2048];
			char cmd_json[256];
			snprintf(sock_path, sizeof(sock_path), "/var/run/rss/%s.sock", target);
			snprintf(cmd_json, sizeof(cmd_json),
				 "{\"cmd\":\"config-get\",\"section\":\"%s\",\"key\":\"%s\"}",
				 section, key);
			int ret = rss_ctrl_send_command(sock_path, cmd_json, resp, sizeof(resp),
							2000);
			if (ret >= 0) {
				printf("%s\n", resp);
				return 0;
			}
		}

		/* Fallback: read from config file */
		const char *cfgpath = "/etc/raptor.conf";
		rss_config_t *cfg = rss_config_load(cfgpath);
		if (!cfg) {
			fprintf(stderr, "Daemon not running, config not found\n");
			return 1;
		}
		const char *val = rss_config_get_str(cfg, section, key, NULL);
		if (val)
			printf("%s\n", val);
		else
			fprintf(stderr, "Key not found: [%s] %s\n", section, key);
		rss_config_free(cfg);
		return val ? 0 : 1;
	}

	/* raptorctl set <section> <key> <value> — write to config file */
	if (strcmp(argv[1], "set") == 0) {
		if (argc < 5) {
			fprintf(stderr, "Usage: raptorctl set <section> <key> <value>\n");
			return 1;
		}
		const char *cfgpath = "/etc/raptor.conf";
		rss_config_t *cfg = rss_config_load(cfgpath);
		if (!cfg) {
			fprintf(stderr, "Failed to load %s\n", cfgpath);
			return 1;
		}
		rss_config_set_str(cfg, argv[2], argv[3], argv[4]);
		int ret = rss_config_save(cfg, cfgpath);
		rss_config_free(cfg);
		if (ret != 0) {
			fprintf(stderr, "Failed to save %s\n", cfgpath);
			return 1;
		}
		return 0;
	}

	/* raptorctl config save — tell all daemons to save */
	if (strcmp(argv[1], "config") == 0) {
		if (argc < 3 || strcmp(argv[2], "save") != 0) {
			fprintf(stderr, "Usage: raptorctl config save\n");
			return 1;
		}
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
		snprintf(json, sizeof(json), "{\"cmd\":\"status\"}");

	} else if (strcmp(cmd, "config") == 0) {
		snprintf(json, sizeof(json), "{\"cmd\":\"config-show\"}");

	} else if (strcmp(cmd, "clients") == 0) {
		snprintf(json, sizeof(json), "{\"cmd\":\"clients\"}");

	} else if (strcmp(cmd, "share-rotate") == 0) {
		snprintf(json, sizeof(json), "{\"cmd\":\"share-rotate\"}");

	} else if (strcmp(cmd, "share") == 0) {
		snprintf(json, sizeof(json), "{\"cmd\":\"share\"}");

	} else if (strcmp(cmd, "request-idr") == 0) {
		if (argc > 3)
			snprintf(json, sizeof(json), "{\"cmd\":\"request-idr\",\"channel\":%s}",
				 argv[3]);
		else
			snprintf(json, sizeof(json), "{\"cmd\":\"request-idr\"}");

	} else if (strcmp(cmd, "set-bitrate") == 0) {
		if (argc < 5) {
			fprintf(stderr, "Usage: raptorctl %s set-bitrate <channel> <bps>\n",
				daemon);
			return 1;
		}
		snprintf(json, sizeof(json),
			 "{\"cmd\":\"set-bitrate\",\"channel\":%s,\"value\":%s}", argv[3], argv[4]);

	} else if (strcmp(cmd, "set-gop") == 0) {
		if (argc < 5) {
			fprintf(stderr, "Usage: raptorctl %s set-gop <channel> <length>\n", daemon);
			return 1;
		}
		snprintf(json, sizeof(json), "{\"cmd\":\"set-gop\",\"channel\":%s,\"value\":%s}",
			 argv[3], argv[4]);

	} else if (strcmp(cmd, "set-fps") == 0) {
		if (argc < 5) {
			fprintf(stderr, "Usage: raptorctl %s set-fps <channel> <fps>\n", daemon);
			return 1;
		}
		snprintf(json, sizeof(json), "{\"cmd\":\"set-fps\",\"channel\":%s,\"value\":%s}",
			 argv[3], argv[4]);

	} else if (strcmp(cmd, "set-qp-bounds") == 0) {
		if (argc < 6) {
			fprintf(stderr, "Usage: raptorctl %s set-qp-bounds <channel> <min> <max>\n",
				daemon);
			return 1;
		}
		snprintf(json, sizeof(json),
			 "{\"cmd\":\"set-qp-bounds\",\"channel\":%s,\"min\":%s,\"max\":%s}",
			 argv[3], argv[4], argv[5]);

	} else if (strcmp(cmd, "set-volume") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl %s set-volume <value>\n", daemon);
			return 1;
		}
		snprintf(json, sizeof(json), "{\"cmd\":\"set-volume\",\"value\":%s}", argv[3]);

	} else if (strcmp(cmd, "set-gain") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl %s set-gain <value>\n", daemon);
			return 1;
		}
		snprintf(json, sizeof(json), "{\"cmd\":\"set-gain\",\"value\":%s}", argv[3]);

	} else if (strcmp(cmd, "ao-set-volume") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl %s ao-set-volume <value>\n", daemon);
			return 1;
		}
		snprintf(json, sizeof(json), "{\"cmd\":\"ao-set-volume\",\"value\":%s}", argv[3]);

	} else if (strcmp(cmd, "ao-set-gain") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl %s ao-set-gain <value>\n", daemon);
			return 1;
		}
		snprintf(json, sizeof(json), "{\"cmd\":\"ao-set-gain\",\"value\":%s}", argv[3]);

	} else if (strcmp(cmd, "set-ns") == 0) {
		if (argc < 4) {
			fprintf(stderr,
				"Usage: raptorctl %s set-ns <0|1> [low|moderate|high|veryhigh]\n",
				daemon);
			return 1;
		}
		if (argc >= 5)
			snprintf(json, sizeof(json),
				 "{\"cmd\":\"set-ns\",\"value\":%s,\"level\":\"%s\"}", argv[3],
				 argv[4]);
		else
			snprintf(json, sizeof(json), "{\"cmd\":\"set-ns\",\"value\":%s}", argv[3]);

	} else if (strcmp(cmd, "set-hpf") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl %s set-hpf <0|1>\n", daemon);
			return 1;
		}
		snprintf(json, sizeof(json), "{\"cmd\":\"set-hpf\",\"value\":%s}", argv[3]);

	} else if (strcmp(cmd, "set-agc") == 0) {
		if (argc < 4) {
			fprintf(stderr,
				"Usage: raptorctl %s set-agc <0|1> [target] [compression]\n",
				daemon);
			return 1;
		}
		if (argc >= 6)
			snprintf(json, sizeof(json),
				 "{\"cmd\":\"set-agc\",\"value\":%s,\"target\":%s,\"compression\":%"
				 "s}",
				 argv[3], argv[4], argv[5]);
		else
			snprintf(json, sizeof(json), "{\"cmd\":\"set-agc\",\"value\":%s}", argv[3]);

	} else if (strcmp(cmd, "privacy") == 0) {
		if (argc > 4)
			snprintf(json, sizeof(json),
				 "{\"cmd\":\"privacy\",\"value\":\"%s\",\"channel\":%ld}", argv[3],
				 strtol(argv[4], NULL, 10));
		else if (argc > 3)
			snprintf(json, sizeof(json), "{\"cmd\":\"privacy\",\"value\":\"%s\"}",
				 argv[3]);
		else
			snprintf(json, sizeof(json), "{\"cmd\":\"privacy\"}");

	} else if (strcmp(cmd, "set-text") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl %s set-text <text>\n", daemon);
			return 1;
		}
		snprintf(json, sizeof(json), "{\"cmd\":\"set-text\",\"value\":\"%s\"}", argv[3]);

	} else if (strcmp(cmd, "mode") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl %s mode <auto|day|night>\n", daemon);
			return 1;
		}
		snprintf(json, sizeof(json), "{\"cmd\":\"mode\",\"value\":\"%s\"}", argv[3]);

	} else if (strcmp(cmd, "isp-mode") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl %s isp-mode <day|night>\n", daemon);
			return 1;
		}
		snprintf(json, sizeof(json), "{\"cmd\":\"isp-mode\",\"value\":\"%s\"}", argv[3]);

	} else if (strcmp(cmd, "set-rc-mode") == 0) {
		if (argc < 5) {
			fprintf(stderr,
				"Usage: raptorctl %s set-rc-mode <ch> <mode> [bitrate]\n"
				"  modes: fixqp cbr vbr smart capped_vbr capped_quality\n",
				daemon);
			return 1;
		}
		int n = snprintf(json, sizeof(json),
				 "{\"cmd\":\"set-rc-mode\",\"channel\":%s,\"mode\":\"%s\"", argv[3],
				 argv[4]);
		if (argc >= 6)
			n += snprintf(json + n, sizeof(json) - n, ",\"bitrate\":%s", argv[5]);
		snprintf(json + n, sizeof(json) - n, "}");

	} else if (strcmp(cmd, "set-wb") == 0) {
		if (argc < 4) {
			fprintf(stderr,
				"Usage: raptorctl %s set-wb <auto|manual> [r_gain] [b_gain]\n",
				daemon);
			return 1;
		}
		int n = snprintf(json, sizeof(json), "{\"cmd\":\"set-wb\",\"mode\":\"%s\"",
				 argv[3]);
		if (argc >= 5)
			n += snprintf(json + n, sizeof(json) - n, ",\"r_gain\":%s", argv[4]);
		if (argc >= 6)
			n += snprintf(json + n, sizeof(json) - n, ",\"b_gain\":%s", argv[5]);
		snprintf(json + n, sizeof(json) - n, "}");

	} else if (strcmp(cmd, "set-font-color") == 0 || strcmp(cmd, "set-stroke-color") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl %s %s <0xAARRGGBB>\n", daemon, cmd);
			return 1;
		}
		snprintf(json, sizeof(json), "{\"cmd\":\"%s\",\"value\":\"%s\"}", cmd, argv[3]);

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
		if (sensor_idx >= 0)
			snprintf(json, sizeof(json), "{\"cmd\":\"%s\",\"value\":%s,\"sensor\":%d}",
				 cmd, val_arg, sensor_idx);
		else
			snprintf(json, sizeof(json), "{\"cmd\":\"%s\",\"value\":%s}", cmd, val_arg);

	} else if (strncmp(cmd, "get-", 4) == 0) {
		/* Generic get-X pass-through */
		snprintf(json, sizeof(json), "{\"cmd\":\"%s\"}", cmd);

	} else {
		/* Generic pass-through */
		snprintf(json, sizeof(json), "{\"cmd\":\"%s\"}", cmd);
	}

	return send_cmd(daemon, json);
}

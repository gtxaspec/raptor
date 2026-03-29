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

static const char *daemons[] = {"rvd", "rsd", "rad", "rod", "rhd", "ric", "rmr", "rmd", "rwd", NULL};

static void usage(void)
{
	fprintf(stderr,
		"Usage: raptorctl <command>\n"
		"\n"
		"Commands:\n"
		"  status                              Show daemon status\n"
		"  memory                              Show memory usage (private/shared)\n"
		"  get <section> <key>                 Read config value\n"
		"  set <section> <key> <value>         Set config value\n"
		"  config save                         Save running config to disk\n"
		"  <daemon> status                     Show daemon details\n"
		"  <daemon> config                     Show running config\n"
		"  <daemon> <cmd> [args...]            Send command\n"
		"\n"
		"RVD commands:\n"
		"  rvd set-bitrate <ch> <bps>          Change bitrate\n"
		"  rvd set-gop <ch> <length>           Change GOP length\n"
		"  rvd set-fps <ch> <fps>              Change frame rate\n"
		"  rvd set-qp-bounds <ch> <min> <max>  Change QP range\n"
		"  rvd request-idr [channel]           Request keyframe\n"
		"  rvd privacy [on|off]                Toggle privacy mode\n"
		"  rvd set-brightness <val>            ISP brightness (0-255)\n"
		"  rvd set-contrast <val>              ISP contrast (0-255)\n"
		"  rvd set-saturation <val>            ISP saturation (0-255)\n"
		"  rvd set-sharpness <val>             ISP sharpness (0-255)\n"
		"  rvd set-hue <val>                   ISP hue (0-255)\n"
		"  rvd set-sinter <val>                Spatial NR (0-255)\n"
		"  rvd set-temper <val>                Temporal NR (0-255)\n"
		"  rvd set-hflip <0|1>                 Horizontal flip\n"
		"  rvd set-vflip <0|1>                 Vertical flip\n"
		"  rvd set-antiflicker <0|1|2>         Off/50Hz/60Hz\n"
		"  rvd set-ae-comp <val>               AE compensation\n"
		"  rvd set-max-again <val>             Max analog gain\n"
		"  rvd set-max-dgain <val>             Max digital gain\n"
		"  rvd set-defog <0|1>                 Defog enable\n"
		"  rvd get-isp                         Show all ISP settings\n"
		"\n"
		"RSD commands:\n"
		"  rsd clients                         List connected RTSP clients\n"
		"\n"
		"RAD commands:\n"
		"  rad set-volume <val>                Change input volume\n"
		"  rad set-gain <val>                  Change input gain\n"
		"  rad set-ns <0|1> [level]            Noise suppression "
		"(low/moderate/high/veryhigh)\n"
		"  rad set-hpf <0|1>                   High-pass filter\n"
		"  rad set-agc <0|1> [target] [comp]   Automatic gain control\n"
		"\n"
		"ROD commands:\n"
		"  rod set-text <text>                 Change OSD text string\n"
		"\n"
		"RIC commands:\n"
		"  ric mode <auto|day|night>           Set day/night mode\n"
		"\n"
		"RMR commands:\n"
		"  rmr status                          Show recording status\n"
		"\n"
		"Testing:\n"
		"  test-motion [sec]                   Trigger clip recording (default 10s)\n"
		"\n"
		"Daemons: rvd, rsd, rad, rod, rhd, ric, rmr, rmd\n");
}

/* Read private and shared memory from /proc/<pid>/smaps.
 * Returns 0 on success, -1 on error. Values in KB. */
static int read_smaps(int pid, long *priv_kb, long *shared_kb)
{
	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/smaps", pid);
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;

	long priv = 0, shared = 0;
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		long val;
		if (sscanf(line, "Private_Clean: %ld kB", &val) == 1)
			priv += val;
		else if (sscanf(line, "Private_Dirty: %ld kB", &val) == 1)
			priv += val;
		else if (sscanf(line, "Shared_Clean: %ld kB", &val) == 1)
			shared += val;
		else if (sscanf(line, "Shared_Dirty: %ld kB", &val) == 1)
			shared += val;
	}
	fclose(f);
	*priv_kb = priv;
	*shared_kb = shared;
	return 0;
}

static void cmd_status(void)
{
	for (int i = 0; daemons[i]; i++) {
		int pid = rss_daemon_check(daemons[i]);
		if (pid > 0)
			printf("%-6s  running (pid %d)\n", daemons[i], pid);
		else
			printf("%-6s  stopped\n", daemons[i]);
	}
}

static void cmd_memory(void)
{
	long total_priv = 0, total_shared = 0;
	int running = 0;

	printf("%-6s  %8s  %8s  %8s\n", "DAEMON", "PRIVATE", "SHARED", "RSS");
	printf("%-6s  %8s  %8s  %8s\n", "------", "-------", "------", "---");

	for (int i = 0; daemons[i]; i++) {
		int pid = rss_daemon_check(daemons[i]);
		if (pid <= 0)
			continue;

		long priv = 0, shared = 0;
		if (read_smaps(pid, &priv, &shared) < 0)
			continue;

		printf("%-6s  %6ld KB  %6ld KB  %6ld KB\n", daemons[i], priv, shared,
		       priv + shared);
		total_priv += priv;
		total_shared += shared;
		running++;
	}

	if (running == 0) {
		printf("No daemons running.\n");
		return;
	}

	/* SHM rings are shared — count them once */
	long shm_total = 0;
	FILE *f = popen("du -sk /dev/shm/rss_ring_* /dev/shm/rss_osd_* 2>/dev/null", "r");
	if (f) {
		char line[256];
		long val;
		while (fgets(line, sizeof(line), f)) {
			if (sscanf(line, "%ld", &val) == 1)
				shm_total += val;
		}
		pclose(f);
	}

	printf("%-6s  %8s  %8s  %8s\n", "------", "-------", "------", "---");
	printf("%-6s  %6ld KB  %6ld KB\n", "TOTAL", total_priv, total_shared);
	printf("\nSHM rings + OSD: %ld KB (shared, counted once)\n", shm_total);
	printf("Actual memory:   %ld KB (private + SHM)\n", total_priv + shm_total);
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
		usage();
		return 1;
	}

	if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
		usage();
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
		usage();
		return 1;
	}

	if (argc < 3) {
		fprintf(stderr, "Missing command for %s\n", argv[1]);
		return 1;
	}

	const char *daemon = argv[1];
	const char *cmd = argv[2];
	char json[512];

	if (strcmp(cmd, "status") == 0) {
		snprintf(json, sizeof(json), "{\"cmd\":\"status\"}");

	} else if (strcmp(cmd, "config") == 0) {
		snprintf(json, sizeof(json), "{\"cmd\":\"config-show\"}");

	} else if (strcmp(cmd, "clients") == 0) {
		snprintf(json, sizeof(json), "{\"cmd\":\"clients\"}");

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
		if (argc > 3)
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

	} else if (strncmp(cmd, "set-", 4) == 0 && argc >= 4) {
		/* Generic set-X <value> pass-through */
		snprintf(json, sizeof(json), "{\"cmd\":\"%s\",\"value\":%s}", cmd, argv[3]);

	} else if (strncmp(cmd, "get-", 4) == 0) {
		/* Generic get-X pass-through */
		snprintf(json, sizeof(json), "{\"cmd\":\"%s\"}", cmd);

	} else {
		/* Generic pass-through */
		snprintf(json, sizeof(json), "{\"cmd\":\"%s\"}", cmd);
	}

	return send_cmd(daemon, json);
}

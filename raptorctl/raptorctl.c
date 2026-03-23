/*
 * raptorctl.c -- Raptor control CLI
 *
 * Usage:
 *   raptorctl status                       Check which daemons are running
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
 *
 * RSD commands:
 *   raptorctl rsd status                   Show client count
 *   raptorctl rsd config                   Show running config
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rss_ipc.h>
#include <rss_common.h>

static const char *daemons[] = { "rvd", "rsd", "rad", "rod", "ric", NULL };

static void usage(void)
{
	fprintf(stderr,
		"Usage: raptorctl <command>\n"
		"\n"
		"Commands:\n"
		"  status                              Show daemon status\n"
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
		"\n"
		"RAD commands:\n"
		"  rad set-volume <val>                Change input volume\n"
		"  rad set-gain <val>                  Change input gain\n"
		"\n"
		"Daemons: rvd, rsd, rad, rod, ric\n");
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
	int ret = rss_ctrl_send_command(sock_path, json,
					resp, sizeof(resp), 5000);
	if (ret < 0) {
		fprintf(stderr, "Failed to send to %s: %s\n",
			daemon, ret == -2 ? "timeout" : "connection failed");
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
			snprintf(sock_path, sizeof(sock_path),
				 "/var/run/rss/%s.sock", daemons[i]);
			int ret = rss_ctrl_send_command(sock_path,
				"{\"cmd\":\"config-save\"}",
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

	} else if (strcmp(cmd, "request-idr") == 0) {
		if (argc > 3)
			snprintf(json, sizeof(json),
				 "{\"cmd\":\"request-idr\",\"channel\":%s}",
				 argv[3]);
		else
			snprintf(json, sizeof(json),
				 "{\"cmd\":\"request-idr\"}");

	} else if (strcmp(cmd, "set-bitrate") == 0) {
		if (argc < 5) {
			fprintf(stderr, "Usage: raptorctl %s set-bitrate <channel> <bps>\n", daemon);
			return 1;
		}
		snprintf(json, sizeof(json),
			 "{\"cmd\":\"set-bitrate\",\"channel\":%s,\"value\":%s}",
			 argv[3], argv[4]);

	} else if (strcmp(cmd, "set-gop") == 0) {
		if (argc < 5) {
			fprintf(stderr, "Usage: raptorctl %s set-gop <channel> <length>\n", daemon);
			return 1;
		}
		snprintf(json, sizeof(json),
			 "{\"cmd\":\"set-gop\",\"channel\":%s,\"value\":%s}",
			 argv[3], argv[4]);

	} else if (strcmp(cmd, "set-fps") == 0) {
		if (argc < 5) {
			fprintf(stderr, "Usage: raptorctl %s set-fps <channel> <fps>\n", daemon);
			return 1;
		}
		snprintf(json, sizeof(json),
			 "{\"cmd\":\"set-fps\",\"channel\":%s,\"value\":%s}",
			 argv[3], argv[4]);

	} else if (strcmp(cmd, "set-qp-bounds") == 0) {
		if (argc < 6) {
			fprintf(stderr, "Usage: raptorctl %s set-qp-bounds <channel> <min> <max>\n", daemon);
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
		snprintf(json, sizeof(json),
			 "{\"cmd\":\"set-volume\",\"value\":%s}", argv[3]);

	} else if (strcmp(cmd, "set-gain") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl %s set-gain <value>\n", daemon);
			return 1;
		}
		snprintf(json, sizeof(json),
			 "{\"cmd\":\"set-gain\",\"value\":%s}", argv[3]);

	} else {
		/* Generic pass-through */
		snprintf(json, sizeof(json), "{\"cmd\":\"%s\"}", cmd);
	}

	return send_cmd(daemon, json);
}

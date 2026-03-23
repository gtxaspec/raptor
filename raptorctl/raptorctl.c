/*
 * raptorctl.c -- Raptor control CLI
 *
 * Usage:
 *   raptorctl status                  Check which daemons are running
 *   raptorctl <daemon> <command>      Send command to daemon
 *
 * Examples:
 *   raptorctl status
 *   raptorctl rvd request-idr
 *   raptorctl rvd set-bitrate 3000000
 *   raptorctl rvd status
 *   raptorctl rsd status
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
		"  status                     Show daemon status\n"
		"  <daemon> <cmd> [args...]   Send command to daemon\n"
		"\n"
		"Daemons: rvd, rsd, rad, rod, ric\n"
		"\n"
		"Examples:\n"
		"  raptorctl status\n"
		"  raptorctl rvd status\n"
		"  raptorctl rvd request-idr\n"
		"  raptorctl rvd set-bitrate 3000000\n");
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

/* Build a JSON command from argv */
static int build_command(int argc, char **argv, int arg_start,
			 char *buf, int buf_size)
{
	const char *cmd = argv[arg_start];

	if (arg_start + 1 < argc) {
		/* Command with value argument */
		snprintf(buf, buf_size,
			 "{\"cmd\":\"%s\",\"value\":%s}",
			 cmd, argv[arg_start + 1]);
	} else {
		/* Command with no args */
		snprintf(buf, buf_size, "{\"cmd\":\"%s\"}", cmd);
	}

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

	/* Daemon command: raptorctl <daemon> <cmd> [args...] */
	if (!is_daemon(argv[1])) {
		fprintf(stderr, "Unknown daemon or command: %s\n", argv[1]);
		usage();
		return 1;
	}

	if (argc < 3) {
		fprintf(stderr, "Missing command for %s\n", argv[1]);
		return 1;
	}

	/* Build socket path */
	char sock_path[64];
	snprintf(sock_path, sizeof(sock_path), "/var/run/rss/%s.sock", argv[1]);

	/* Build JSON command */
	char cmd_json[256];
	build_command(argc, argv, 2, cmd_json, sizeof(cmd_json));

	/* Send command */
	char resp[1024];
	int ret = rss_ctrl_send_command(sock_path, cmd_json,
					resp, sizeof(resp), 5000);
	if (ret < 0) {
		fprintf(stderr, "Failed to send command to %s: %s\n",
			argv[1],
			ret == -2 ? "timeout" : "connection failed");
		return 1;
	}

	printf("%s\n", resp);
	return 0;
}

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

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <rss_ipc.h>
#include <rss_common.h>

static const char *daemons[] = {"rvd", "rsd", "rad", "rod", "rhd", "ric",
				"rmr", "rmd", "rwd", "rwc", NULL};

/* Per-daemon help entries. NULL daemon = global commands. */
struct help_entry {
	const char *daemon;
	const char *text;
};

static const struct help_entry help_entries[] = {
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
	{"ric", "mode <auto|day|night>               Set day/night mode"},
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

/* Raptor threads use pthread_attr_setstacksize (128KB or 64KB).
 * SDK/library threads use the default (~2MB). Threshold to distinguish. */
#define RAPTOR_STACK_THRESHOLD 256 /* KB — anything above is SDK */

/* Sum all per-thread stack sizes via /proc/pid/task/tid/smaps.
 * Each thread has an anonymous [stack] mapping with Size and Rss.
 * Splits into raptor vs SDK based on stack allocation size. */
typedef struct {
	long raptor_alloc, raptor_used;
	long sdk_alloc, sdk_used;
} stack_info_t;

static void read_total_stack(int pid, stack_info_t *info)
{
	memset(info, 0, sizeof(*info));

	char task_dir[64];
	snprintf(task_dir, sizeof(task_dir), "/proc/%d/task", pid);
	DIR *d = opendir(task_dir);
	if (!d)
		return;

	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		if (ent->d_name[0] == '.')
			continue;
		char smaps_path[128];
		snprintf(smaps_path, sizeof(smaps_path), "%s/%s/smaps", task_dir, ent->d_name);
		FILE *f = fopen(smaps_path, "r");
		if (!f)
			continue;
		char line[256];
		int in_stack = 0;
		long sz = 0, rss = 0;
		while (fgets(line, sizeof(line), f)) {
			if (strstr(line, "[stack]")) {
				in_stack = 1;
				sz = rss = 0;
			} else if (in_stack) {
				long val;
				if (sscanf(line, "Size: %ld kB", &val) == 1) {
					sz = val;
				} else if (sscanf(line, "Rss: %ld kB", &val) == 1) {
					rss = val;
					if (sz > RAPTOR_STACK_THRESHOLD) {
						info->sdk_alloc += sz;
						info->sdk_used += rss;
					} else {
						info->raptor_alloc += sz;
						info->raptor_used += rss;
					}
					in_stack = 0;
				}
			}
		}
		fclose(f);
	}
	closedir(d);
}

/* Read utime + stime (in clock ticks) from /proc/<pid>/stat.
 * Fields 14 and 15 (1-indexed) in the stat line. Also reads
 * num_threads (field 20) and vsize (field 23, in bytes). */
static int read_proc_stat(int pid, unsigned long *ticks, int *threads, unsigned long *vsize)
{
	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;

	char line[1024];
	if (!fgets(line, sizeof(line), f)) {
		fclose(f);
		return -1;
	}
	fclose(f);

	/* Skip past comm field "(name)" which may contain spaces */
	char *p = strrchr(line, ')');
	if (!p)
		return -1;
	p += 2; /* skip ") " */

	/* Fields after comm (1-indexed):
	 * 3:state 4:ppid 5:pgrp 6:session 7:tty 8:tpgid 9:flags
	 * 10:minflt 11:cminflt 12:majflt 13:cmajflt
	 * 14:utime 15:stime 16:cutime 17:cstime
	 * 18:priority 19:nice 20:num_threads 21:itrealvalue
	 * 22:starttime 23:vsize 24:rss */
	unsigned long utime = 0, stime = 0, vs = 0;
	int thr = 0;
	int n = sscanf(p,
		       "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u "
		       "%lu %lu %*d %*d %*d %*d %d %*d %*u %lu",
		       &utime, &stime, &thr, &vs);
	if (n < 4)
		return -1;

	*ticks = utime + stime;
	*threads = thr;
	*vsize = vs;
	return 0;
}

/* Read total CPU ticks from /proc/stat (all cores combined) */
static unsigned long read_total_cpu_ticks(void)
{
	FILE *f = fopen("/proc/stat", "r");
	if (!f)
		return 0;
	char line[256];
	unsigned long user = 0, nice = 0, sys = 0, idle = 0, iow = 0, irq = 0, sirq = 0;
	if (fgets(line, sizeof(line), f))
		sscanf(line, "cpu %lu %lu %lu %lu %lu %lu %lu", &user, &nice, &sys, &idle, &iow,
		       &irq, &sirq);
	fclose(f);
	return user + nice + sys + idle + iow + irq + sirq;
}

static void cmd_cpu(void)
{
	/* First sample */
	struct {
		int pid;
		unsigned long ticks;
		int threads;
		unsigned long vsize;
	} s1[16] = {0}, s2[16] = {0};
	int count = 0;

	unsigned long total1 = read_total_cpu_ticks();
	for (int i = 0; daemons[i] && count < 16; i++) {
		int pid = rss_daemon_check(daemons[i]);
		if (pid <= 0)
			continue;
		s1[count].pid = pid;
		read_proc_stat(pid, &s1[count].ticks, &s1[count].threads, &s1[count].vsize);
		count++;
	}

	if (count == 0) {
		printf("No daemons running.\n");
		return;
	}

	/* Wait 1 second */
	usleep(1000000);

	/* Second sample */
	unsigned long total2 = read_total_cpu_ticks();
	unsigned long total_delta = total2 - total1;
	if (total_delta == 0)
		total_delta = 1;

	int idx = 0;
	for (int i = 0; daemons[i] && idx < count; i++) {
		int pid = rss_daemon_check(daemons[i]);
		if (pid <= 0 || pid != s1[idx].pid)
			continue;
		read_proc_stat(pid, &s2[idx].ticks, &s2[idx].threads, &s2[idx].vsize);
		idx++;
	}

	printf("%-6s  %6s  %7s\n", "DAEMON", "CPU %", "THREADS");
	printf("%-6s  %6s  %7s\n", "------", "-----", "-------");

	double total_cpu = 0;
	idx = 0;
	for (int i = 0; daemons[i] && idx < count; i++) {
		int pid = rss_daemon_check(daemons[i]);
		if (pid <= 0 || pid != s1[idx].pid)
			continue;
		unsigned long delta = s2[idx].ticks - s1[idx].ticks;
		double pct = 100.0 * (double)delta / (double)total_delta;
		total_cpu += pct;
		printf("%-6s  %5.1f%%  %7d\n", daemons[i], pct, s2[idx].threads);
		idx++;
	}
	printf("%-6s  %6s  %7s\n", "------", "-----", "-------");
	printf("%-6s  %5.1f%%\n", "TOTAL", total_cpu);
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

#define MEM_HDR "%-6s  %9s  %9s  %9s  %13s  %13s  %9s\n"
	printf(MEM_HDR, "DAEMON", "PRIVATE", "SHARED", "RSS", "STACK raptor", "STACK sdk", "VSIZE");
	printf(MEM_HDR, "------", "-------", "------", "---", "------------", "---------", "-----");

	for (int i = 0; daemons[i]; i++) {
		int pid = rss_daemon_check(daemons[i]);
		if (pid <= 0)
			continue;

		long priv = 0, shared = 0;
		if (read_smaps(pid, &priv, &shared) < 0)
			continue;

		unsigned long ticks, vsize;
		int threads;
		read_proc_stat(pid, &ticks, &threads, &vsize);
		stack_info_t stk;
		read_total_stack(pid, &stk);

		printf("%-6s  %6ld KB  %6ld KB  %6ld KB  %3ld/%-5ld KB  %3ld/%-5ld KB  %6lu KB\n",
		       daemons[i], priv, shared, priv + shared, stk.raptor_used, stk.raptor_alloc,
		       stk.sdk_used, stk.sdk_alloc, vsize / 1024);
		total_priv += priv;
		total_shared += shared;
		running++;
	}

	if (running == 0) {
		printf("No daemons running.\n");
		return;
	}

	/* SHM breakdown — list individual rings and OSD buffers */
	long shm_rings = 0, shm_osd = 0;

	printf(MEM_HDR, "------", "-------", "------", "---", "------------", "---------", "-----");
	printf("%-6s  %6ld KB  %6ld KB\n", "TOTAL", total_priv, total_shared);

	printf("\nSHM rings:\n");
	FILE *f = popen("ls -l /dev/shm/rss_ring_* 2>/dev/null", "r");
	if (f) {
		char line[512];
		while (fgets(line, sizeof(line), f)) {
			long sz;
			char name[128];
			/* ls -l: perms links owner group size date date date name */
			if (sscanf(line, "%*s %*s %*s %*s %ld %*s %*s %*s %127s", &sz, name) == 2) {
				const char *base = strrchr(name, '/');
				base = base ? base + 1 : name;
				/* strip rss_ring_ prefix */
				const char *label = base;
				if (strncmp(label, "rss_ring_", 9) == 0)
					label += 9;
				printf("  %-20s %6ld KB\n", label, sz / 1024);
				shm_rings += sz / 1024;
			}
		}
		pclose(f);
	}

	printf("OSD buffers:\n");
	f = popen("ls -l /dev/shm/rss_osd_* 2>/dev/null", "r");
	if (f) {
		char line[512];
		while (fgets(line, sizeof(line), f)) {
			long sz;
			char name[128];
			if (sscanf(line, "%*s %*s %*s %*s %ld %*s %*s %*s %127s", &sz, name) == 2) {
				const char *base = strrchr(name, '/');
				base = base ? base + 1 : name;
				const char *label = base;
				if (strncmp(label, "rss_osd_", 8) == 0)
					label += 8;
				printf("  %-20s %6ld KB\n", label, sz / 1024);
				shm_osd += sz / 1024;
			}
		}
		pclose(f);
	}

	long shm_total = shm_rings + shm_osd;
	printf("SHM total: %ld KB (rings %ld KB + OSD %ld KB)\n", shm_total, shm_rings, shm_osd);
	printf("Actual memory: %ld KB (private + SHM)\n", total_priv + shm_total);
	printf("Note: STACK is included in PRIVATE (not additional)\n");
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

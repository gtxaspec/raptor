/*
 * raptorctl_info.c -- System info commands (status, memory, cpu)
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <rss_ipc.h>

#include <rss_common.h>

#include "raptorctl.h"

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

/* Sum all per-thread stack sizes via /proc/pid/task/tid/smaps.
 * Each thread has an anonymous [stack] mapping with Size and Rss.
 * Splits into raptor vs SDK based on stack allocation size. */
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
		char smaps_path[512];
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
					break; /* one [stack] per thread */
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

void cmd_cpu(void)
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

void cmd_status(void)
{
	for (int i = 0; daemons[i]; i++) {
		int pid = rss_daemon_check(daemons[i]);
		if (pid > 0)
			printf("%-6s  running (pid %d)\n", daemons[i], pid);
		else
			printf("%-6s  stopped\n", daemons[i]);
	}
}

void cmd_memory(void)
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

		printf("%-6s  %6ld KB  %6ld KB  %6ld KB  %4ld/%5ld KB  %4ld/%5ld KB  %6lu KB\n",
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

	/* SHM breakdown -- list individual rings and OSD buffers */
	long shm_rings = 0, shm_osd = 0;

	printf(MEM_HDR, "------", "-------", "------", "---", "------------", "---------", "-----");
	printf("%-6s  %6ld KB  %6ld KB\n", "TOTAL", total_priv, total_shared);

	/* List SHM files with opendir/stat instead of popen("ls -l ...") */
	DIR *dir = opendir(RSS_SHM_DIR);
	if (dir) {
		struct dirent *ent;

		printf("\nSHM rings:\n");
		while ((ent = readdir(dir))) {
			if (strncmp(ent->d_name, "rss_ring_", 9) != 0)
				continue;
			const char *ring_name = ent->d_name + 9;
			char path[280];
			snprintf(path, sizeof(path), RSS_SHM_DIR "/%s", ent->d_name);
			struct stat st;
			if (stat(path, &st) != 0)
				continue;
			long sz = (long)st.st_size / 1024;
			rss_ring_t *ring = rss_ring_open(ring_name);
			if (ring) {
				uint32_t rv;
				bool ok = rss_ring_version_ok(ring, &rv);
				printf("  %-20s %6ld KB  v%u%s\n", ring_name, sz, rv,
				       ok ? "" : " (MISMATCH — rebuild producer)");
				rss_ring_close(ring);
			} else {
				printf("  %-20s %6ld KB\n", ring_name, sz);
			}
			shm_rings += sz;
		}

		rewinddir(dir);

		printf("OSD buffers:\n");
		while ((ent = readdir(dir))) {
			if (strncmp(ent->d_name, "rss_osd_", 8) != 0)
				continue;
			char path[280];
			snprintf(path, sizeof(path), RSS_SHM_DIR "/%s", ent->d_name);
			struct stat st;
			if (stat(path, &st) != 0)
				continue;
			long sz = (long)st.st_size / 1024;
			printf("  %-20s %6ld KB\n", ent->d_name + 8, sz);
			shm_osd += sz;
		}

		closedir(dir);
	}

	long shm_total = shm_rings + shm_osd;
	printf("SHM total: %ld KB (rings %ld KB + OSD %ld KB)\n", shm_total, shm_rings, shm_osd);
	printf("Actual memory: %ld KB (private + SHM)\n", total_priv + shm_total);
	printf("Note: STACK is included in PRIVATE (not additional)\n");
}

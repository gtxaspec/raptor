/*
 * rmr_storage.c -- Recording file rotation and storage management
 *
 * Directory layout: {base_path}/YYYY-MM-DD/[HH/]HH-MM-SS.mp4 (hour level with hour_dirs)
 * One directory per day. Cleanup deletes the oldest segments first.
 */

#include "rmr_storage.h"

#include <rss_common.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

struct rmr_storage {
	char base_path[256];
	int segment_minutes;
	int segment_seconds;
	int max_storage_mb;
	int min_free_mb;
	/* base_path follows the largest card. The picker thread reads
	 * /proc/mounts and statvfs()s every card mount, which can block for
	 * seconds on a dying card: never on the frame path. It hands its
	 * pick over in picked; the main loop applies it. */
	bool auto_root;
	char subdir[32];
	pthread_t pick_thr;
	pthread_mutex_t pick_lock; /* never held across a pick */
	pthread_cond_t pick_cv;
	bool pick_started, pick_stop, pick_now;
	int repick_ms;
	char picked[256];  /* under pick_lock */
	bool picked_new;   /* set by the picker, taken by the main loop */
	char pending[256]; /* main loop: picked, not applied yet */
	bool pending_set;
	int64_t next_wake_us;
	bool root_checked, root_small; /* main loop: size of the root, once per mount */
	bool small_warned;	       /* main loop: the small-root warning, once per root */
	/* cleanup off the main loop: deleting 100+ MB files on a busy card
	 * took up to 0.8 s there, enough to overflow a 32-slot ring */
	pthread_mutex_t clean_lock;
	pthread_mutex_t req_lock;
	pthread_cond_t req_cv;
	pthread_t clean_thr;
	bool clean_started, clean_pending, clean_stop;
	int cleanup_delay_ms;
	bool hour_dirs;
	bool pacing;		   /* set while the thread runs a delayed cleanup */
	bool refuse_logged;	   /* rootfs auto-create refusal logged once */
	int64_t last_wait_warn_us; /* rate-limit for the waiting log       */
};

static const char *pick_mounts = "/proc/mounts";
static const char *pick_prefix = "/mnt/";
static rmr_statvfs_fn pick_fs = statvfs;
static const char *pick_fallback = RMR_STORAGE_DEFAULT_ROOT;

void rmr_storage_set_pick_source(const char *mounts, const char *mnt_prefix, rmr_statvfs_fn fs,
				 const char *fallback_root)
{
	pick_mounts = mounts ? mounts : "/proc/mounts";
	pick_prefix = mnt_prefix ? mnt_prefix : "/mnt/";
	pick_fs = fs ? fs : statvfs;
	pick_fallback = fallback_root ? fallback_root : RMR_STORAGE_DEFAULT_ROOT;
}

static void *pick_thread(void *arg);

rmr_storage_t *rmr_storage_create(const rmr_storage_config_t *cfg)
{
	if (!cfg)
		return NULL;

	rmr_storage_t *st = calloc(1, sizeof(*st));
	if (!st)
		return NULL;

	if (cfg->base_path && cfg->base_path[0]) {
		snprintf(st->base_path, sizeof(st->base_path), "%s", cfg->base_path);
	} else {
		char root[sizeof(st->base_path) - sizeof(st->subdir)];
		st->auto_root = true;
		snprintf(st->subdir, sizeof(st->subdir), "%s", cfg->subdir ? cfg->subdir : "");
		rmr_storage_pick_root(root, sizeof(root));
		snprintf(st->base_path, sizeof(st->base_path), "%s%s", root, st->subdir);
	}
	st->segment_minutes = cfg->segment_minutes > 0 ? cfg->segment_minutes : 5;
	st->segment_seconds = cfg->segment_seconds;
	st->max_storage_mb = cfg->max_storage_mb;
	st->min_free_mb = cfg->min_free_mb;
	st->cleanup_delay_ms = cfg->cleanup_delay_ms;
	st->hour_dirs = cfg->hour_dirs;
	pthread_mutex_init(&st->clean_lock, NULL);
	pthread_mutex_init(&st->req_lock, NULL);
	pthread_cond_init(&st->req_cv, NULL);
	pthread_mutex_init(&st->pick_lock, NULL);
	/* monotonic: NTP steps the wall clock back at boot, which would
	 * stall a CLOCK_REALTIME wait by as much */
	pthread_condattr_t ca;
	pthread_condattr_init(&ca);
	pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
	pthread_cond_init(&st->pick_cv, &ca);
	pthread_condattr_destroy(&ca);
	if (st->auto_root) {
		st->repick_ms = cfg->repick_ms > 0 ? cfg->repick_ms : 10000;
		snprintf(st->picked, sizeof(st->picked), "%s", st->base_path);
		pthread_attr_t ta;
		pthread_attr_init(&ta);
		pthread_attr_setstacksize(&ta, 64 * 1024);
		st->pick_started = pthread_create(&st->pick_thr, &ta, pick_thread, st) == 0;
		pthread_attr_destroy(&ta);
	}

	return st;
}

void rmr_storage_destroy(rmr_storage_t *st)
{
	if (!st)
		return;
	if (st->pick_started) {
		pthread_mutex_lock(&st->pick_lock);
		st->pick_stop = true;
		pthread_cond_signal(&st->pick_cv);
		pthread_mutex_unlock(&st->pick_lock);
		pthread_join(st->pick_thr, NULL);
	}
	if (st->clean_started) {
		pthread_mutex_lock(&st->req_lock);
		__atomic_store_n(&st->clean_stop, true, __ATOMIC_RELAXED);
		pthread_cond_signal(&st->req_cv);
		pthread_mutex_unlock(&st->req_lock);
		pthread_join(st->clean_thr, NULL);
	}
	pthread_cond_destroy(&st->req_cv);
	pthread_mutex_destroy(&st->req_lock);
	pthread_mutex_destroy(&st->clean_lock);
	pthread_cond_destroy(&st->pick_cv);
	pthread_mutex_destroy(&st->pick_lock);
	free(st);
}

int rmr_storage_open_file(rmr_storage_t *st, int64_t rt_us, const char *suffix, char *path_out,
			  int path_out_size)
{
	if (!st)
		return -1;

	time_t t = (time_t)(rt_us / 1000000);
	struct tm tm;
	localtime_r(&t, &tm);

	/* {base}/YYYY-MM-DD/[HH/] */
	char dir[320];
	int dn = snprintf(dir, sizeof(dir), "%s/%04d-%02d-%02d", st->base_path, tm.tm_year + 1900,
			  tm.tm_mon + 1, tm.tm_mday);
	if (st->hour_dirs && dn > 0 && (size_t)dn < sizeof(dir))
		snprintf(dir + dn, sizeof(dir) - (size_t)dn, "/%02d", tm.tm_hour);
	rss_mkdir_p(dir);

	/* Filename: HH-MM-SS.mp4, HH-MM-SS_<suffix>.mp4. A name already taken
	 * (a resume within the same second, whose predecessor may still have
	 * writes queued on the writer thread) gets .1 to .9 before .mp4:
	 * truncating it would mix the two files in one inode. */
	bool tagged = suffix && suffix[0];
	int fd = -1;
	for (int n = 0; n < 10; n++) {
		char dup[3] = "";
		if (n) {
			dup[0] = '.';
			dup[1] = (char)('0' + n);
		}
		snprintf(path_out, path_out_size, "%s/%02d-%02d-%02d%s%s%s.mp4", dir, tm.tm_hour,
			 tm.tm_min, tm.tm_sec, tagged ? "_" : "", tagged ? suffix : "", dup);
		fd = open(path_out, O_WRONLY | O_CREAT | O_EXCL, 0644);
		if (fd < 0 && errno == ENOSPC && rmr_storage_enforce_limit(st) > 0)
			fd = open(path_out, O_WRONLY | O_CREAT | O_EXCL, 0644);
		if (fd >= 0 || errno != EEXIST)
			break;
	}
	if (fd < 0)
		RSS_ERROR("failed to open segment: %s: %m", path_out);

	return fd;
}

int rmr_storage_open_segment(rmr_storage_t *st, char *path_out, int path_out_size)
{
	return rmr_storage_open_file(st, (int64_t)time(NULL) * 1000000, NULL, path_out,
				     path_out_size);
}

int64_t rmr_name_stamp_take(rmr_name_stamp_t *s, uint32_t *used, int64_t now, int64_t rt_us,
			    int64_t until)
{
	if (s->gen != 0 && *used != s->gen && now < s->until) {
		*used = s->gen;
		return s->rt_us;
	}
	s->gen = s->gen + 1 ? s->gen + 1 : 1;
	s->rt_us = rt_us;
	s->until = until;
	*used = s->gen;
	return rt_us;
}

void rmr_storage_close_segment(int fd)
{
	if (fd >= 0) {
		fsync(fd);
		close(fd);
	}
}

int64_t rmr_storage_next_boundary(int64_t now_rt_us, int segment_len_sec)
{
	int64_t period = (int64_t)segment_len_sec * 1000000LL;
	return (now_rt_us / period + 1) * period;
}

int rmr_storage_segment_len_sec(rmr_storage_t *st)
{
	if (!st)
		return 300;
	return st->segment_seconds > 0 ? st->segment_seconds : st->segment_minutes * 60;
}

#define RMR_MIN_SEGMENT_US (5 * 1000000LL)

bool rmr_storage_should_rotate_at(rmr_storage_t *st, int64_t start_rt_us, int64_t now_rt_us)
{
	if (!st)
		return false;
	if (now_rt_us - start_rt_us < RMR_MIN_SEGMENT_US)
		return false;
	return now_rt_us >= rmr_storage_next_boundary(start_rt_us, rmr_storage_segment_len_sec(st));
}

/*
 * Auto-create base_path, but only when it lands on a different
 * filesystem than "/". The default path lives under an SD mountpoint;
 * with the card absent the mountpoint directory sits on the root
 * (overlay) filesystem and a blind mkdir -p would silently record
 * onto flash. Comparing the deepest existing ancestor's st_dev
 * against "/" allows mounted media (SD, NFS, tmpfs) and refuses the
 * bare rootfs.
 */
static bool storage_try_create(rmr_storage_t *st)
{
	char anc[sizeof(st->base_path)];
	snprintf(anc, sizeof(anc), "%s", st->base_path);

	struct stat s;
	while (stat(anc, &s) != 0) {
		char *slash = strrchr(anc, '/');
		if (!slash)
			return false;
		if (slash == anc) {
			anc[1] = '\0'; /* reached "/" */
			break;
		}
		*slash = '\0';
	}
	if (stat(anc, &s) != 0)
		return false;

	struct stat root;
	if (stat("/", &root) != 0)
		return false;

	if (s.st_dev == root.st_dev) {
		if (!st->refuse_logged) {
			RSS_WARN("storage %s is on the root filesystem — not auto-creating "
				 "(mount media or create the directory manually)",
				 st->base_path);
			st->refuse_logged = true;
		}
		return false;
	}

	rss_mkdir_p(st->base_path);
	if (access(st->base_path, W_OK) != 0)
		return false;

	RSS_INFO("created storage directory: %s", st->base_path);
	st->refuse_logged = false;
	return true;
}

int rmr_storage_pick_root(char *out, int out_size)
{
	uint64_t best = 0;
	char line[512], dev[128], mnt[256];
	FILE *f = fopen(pick_mounts, "r");

	out[0] = '\0';
	while (f && fgets(line, sizeof(line), f)) {
		struct statvfs v;
		if (sscanf(line, "%127s %255s", dev, mnt) != 2 ||
		    strncmp(dev, "/dev/mmcblk", 11) != 0 ||
		    strncmp(mnt, pick_prefix, strlen(pick_prefix)) != 0 || pick_fs(mnt, &v) != 0)
			continue;
		uint64_t size = (uint64_t)v.f_blocks * v.f_frsize;
		if (size < RMR_AUTO_ROOT_MIN_MB * 1024ULL * 1024ULL)
			continue;
		if (size > best) {
			best = size;
			snprintf(out, out_size, "%s/raptor", mnt);
		}
	}
	if (f)
		fclose(f);
	if (out[0])
		return 0;
	snprintf(out, out_size, "%s", pick_fallback);
	return -1;
}

bool rmr_storage_loops(const rmr_storage_t *st)
{
	return st && (st->max_storage_mb > 0 || st->min_free_mb > 0);
}

const char *rmr_storage_path(const rmr_storage_t *st)
{
	return st ? st->base_path : "";
}

/* Look for the largest card partition every repick_ms, and at once when
 * the main loop finds its root unusable. A card mounts its partitions one
 * by one, so the first to appear after a reinsert need not be the largest. */
static void *pick_thread(void *arg)
{
	rmr_storage_t *st = arg;
	pthread_mutex_lock(&st->pick_lock);
	while (!st->pick_stop) {
		if (!st->pick_now) {
			struct timespec until;
			clock_gettime(CLOCK_MONOTONIC, &until);
			until.tv_sec += st->repick_ms / 1000;
			until.tv_nsec += (long)(st->repick_ms % 1000) * 1000000L;
			if (until.tv_nsec >= 1000000000L) {
				until.tv_sec++;
				until.tv_nsec -= 1000000000L;
			}
			pthread_cond_timedwait(&st->pick_cv, &st->pick_lock, &until);
			if (st->pick_stop)
				break;
		}
		st->pick_now = false;
		pthread_mutex_unlock(&st->pick_lock);

		char root[sizeof(st->base_path) - sizeof(st->subdir)], path[sizeof(st->base_path)];
		/* no card big enough: keep the current root and wait there,
		 * rather than fall back to what may be the small partition */
		bool found = rmr_storage_pick_root(root, sizeof(root)) == 0;
		snprintf(path, sizeof(path), "%s%s", root, st->subdir);

		pthread_mutex_lock(&st->pick_lock);
		if (found && strcmp(path, st->picked) != 0) {
			snprintf(st->picked, sizeof(st->picked), "%s", path);
			__atomic_store_n(&st->picked_new, true, __ATOMIC_RELEASE);
		}
	}
	pthread_mutex_unlock(&st->pick_lock);
	return NULL;
}

/* Main loop: take the picker's choice. The cleanup thread walks base_path
 * under clean_lock and a paced pass holds it for minutes, so never wait on
 * it here: a busy lock leaves the change pending for the next call, and
 * the next segment opens on the new root. */
static void storage_take_pick(rmr_storage_t *st)
{
	if (__atomic_load_n(&st->picked_new, __ATOMIC_ACQUIRE)) {
		pthread_mutex_lock(&st->pick_lock);
		snprintf(st->pending, sizeof(st->pending), "%s", st->picked);
		st->picked_new = false;
		pthread_mutex_unlock(&st->pick_lock);
		st->pending_set = true;
	}
	if (!st->pending_set)
		return;
	if (strcmp(st->pending, st->base_path) == 0) {
		st->pending_set = false;
		return;
	}
	if (pthread_mutex_trylock(&st->clean_lock) != 0)
		return;
	RSS_INFO("storage: %s", st->pending);
	snprintf(st->base_path, sizeof(st->base_path), "%s", st->pending);
	st->refuse_logged = false;
	st->root_checked = false;
	st->small_warned = false;
	st->pending_set = false;
	pthread_mutex_unlock(&st->clean_lock);
}

/* Root unusable: have the picker look now rather than at its next period. */
static void storage_wake_picker(rmr_storage_t *st)
{
	int64_t now = rss_timestamp_us();
	if (!st->pick_started || now < st->next_wake_us)
		return;
	st->next_wake_us = now + 1000000LL;
	pthread_mutex_lock(&st->pick_lock);
	st->pick_now = true;
	pthread_cond_signal(&st->pick_cv);
	pthread_mutex_unlock(&st->pick_lock);
}

/* Is path on a card partition (/dev/mmcblk* in the pick mount table)? */
static bool on_card_mount(const char *path)
{
	char line[512], dev[128], mnt[256];
	bool on = false;
	FILE *f = fopen(pick_mounts, "r");
	while (f && !on && fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%127s %255s", dev, mnt) != 2 ||
		    strncmp(dev, "/dev/mmcblk", 11) != 0)
			continue;
		size_t n = strlen(mnt);
		on = strncmp(path, mnt, n) == 0 && (path[n] == '\0' || path[n] == '/');
	}
	if (f)
		fclose(f);
	return on;
}

/* Is the file system under the auto root smaller than RMR_AUTO_ROOT_MIN_MB?
 * Sized on the deepest existing directory (the mount point before the
 * root is created). Cached once the root is writable; until then the
 * main loop only polls about once a second. Warns once per root. */
static bool root_is_small(rmr_storage_t *st, bool writable)
{
	if (st->root_checked)
		return st->root_small;
	char anc[sizeof(st->base_path)];
	snprintf(anc, sizeof(anc), "%s", st->base_path);
	struct stat sb;
	while (stat(anc, &sb) != 0) {
		char *slash = strrchr(anc, '/');
		if (!slash || slash == anc)
			return false;
		*slash = '\0';
	}
	/* no card at all leaves only rootfs directories here: that is a
	 * missing card, for the rootfs refusal and the waiting log to report */
	if (!on_card_mount(anc))
		return false;
	struct statvfs v;
	bool small = pick_fs(anc, &v) == 0 &&
		     (uint64_t)v.f_blocks * v.f_frsize < RMR_AUTO_ROOT_MIN_MB * 1024ULL * 1024ULL;
	if (small && !st->small_warned) {
		RSS_WARN("storage: %s is under %d MB, not recording there", st->base_path,
			 RMR_AUTO_ROOT_MIN_MB);
		st->small_warned = true;
	}
	if (writable) {
		st->root_checked = true;
		st->root_small = small;
	}
	return small;
}

bool rmr_storage_available(rmr_storage_t *st)
{
	if (!st)
		return false;
	if (st->auto_root)
		storage_take_pick(st);
	bool ok = access(st->base_path, W_OK) == 0;
	if (!ok && st->auto_root) {
		st->root_checked = false;
		storage_wake_picker(st);
	}
	/* the fallback root, picked when no card was big enough, can be the
	 * small partition itself: stand by instead of filling it, and check
	 * before auto-creating anything there */
	if (st->auto_root && root_is_small(st, ok)) {
		storage_wake_picker(st);
		return false;
	}
	if (ok || storage_try_create(st))
		return true;

	int64_t now = rss_timestamp_us();
	if (now - st->last_wait_warn_us >= 60000000LL) {
		RSS_WARN("waiting for storage: %s", st->base_path);
		st->last_wait_warn_us = now;
	}
	return false;
}

/* ── Storage cleanup ── */

/* Compare strings for qsort (alphabetical = chronological for our naming) */
static int str_cmp(const void *a, const void *b)
{
	return strcmp(*(const char **)a, *(const char **)b);
}

/*
 * Scan a directory and return sorted list of entry names.
 * Caller must free each name and the array.
 */
static char **scan_dir_sorted(const char *path, int *count)
{
	DIR *d = opendir(path);
	if (!d) {
		*count = 0;
		return NULL;
	}

	int cap = 64;
	int n = 0;
	char **names = malloc(cap * sizeof(char *));
	if (!names) {
		closedir(d);
		*count = 0;
		return NULL;
	}

	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		if (ent->d_name[0] == '.')
			continue;
		if (n >= cap) {
			cap *= 2;
			char **tmp = realloc(names, cap * sizeof(char *));
			if (!tmp)
				break; /* use what we have */
			names = tmp;
		}
		char *dup = strdup(ent->d_name);
		if (!dup)
			break;
		names[n++] = dup;
	}
	closedir(d);

	if (n > 1)
		qsort(names, n, sizeof(char *), str_cmp);

	*count = n;
	return names;
}

static void free_names(char **names, int count)
{
	for (int i = 0; i < count; i++)
		free(names[i]);
	free(names);
}

static bool is_mp4(const char *n);

static bool is_hour_dir(const char *n)
{
	return strlen(n) == 2 && n[0] >= '0' && n[0] <= '2' && n[1] >= '0' && n[1] <= '9';
}

typedef struct {
	size_t name_off; /* basename in path; an offset survives realloc and qsort */
	char path[1024];
} day_file_t;

#define DF_NAME(f) ((f)->path + (f)->name_off)

static int day_file_cmp(const void *a, const void *b)
{
	return strcmp(DF_NAME((const day_file_t *)a), DF_NAME((const day_file_t *)b));
}

/* Files of one day, flat or in hour subdirectories, oldest name first.
 * Names are HH-MM-SS[_tag].mp4 in both layouts, so they sort together. */
static day_file_t *collect_day(const char *day_path, int *count)
{
	int n = 0, cap = 64;
	day_file_t *out = malloc(cap * sizeof(*out));
	int top_n;
	char **top = scan_dir_sorted(day_path, &top_n);
	*count = 0;
	if (!out || !top) {
		free(out);
		if (top)
			free_names(top, top_n);
		return NULL;
	}
	for (int i = 0; i < top_n; i++) {
		char sub[768];
		snprintf(sub, sizeof(sub), "%s/%s", day_path, top[i]);
		int sub_n = 0;
		char **names = NULL;
		bool hour = is_hour_dir(top[i]);
		if (hour)
			names = scan_dir_sorted(sub, &sub_n);
		int m = hour ? sub_n : 1;
		for (int k = 0; k < m; k++) {
			if (n == cap) {
				day_file_t *t = realloc(out, (size_t)cap * 2 * sizeof(*out));
				if (!t)
					break;
				out = t;
				cap *= 2;
			}
			if (hour)
				snprintf(out[n].path, sizeof(out[n].path), "%s/%s", sub, names[k]);
			else
				snprintf(out[n].path, sizeof(out[n].path), "%s", sub);
			out[n].name_off = (size_t)(strrchr(out[n].path, '/') + 1 - out[n].path);
			n++;
		}
		if (names)
			free_names(names, sub_n);
	}
	free_names(top, top_n);
	if (n > 1)
		qsort(out, (size_t)n, sizeof(*out), day_file_cmp);
	*count = n;
	return out;
}

/* Remove a day's emptied hour directories, then the day itself */
static void prune_day(const char *day_path)
{
	int n;
	char **names = scan_dir_sorted(day_path, &n);
	for (int i = 0; names && i < n; i++) {
		if (!is_hour_dir(names[i]))
			continue;
		char sub[768];
		snprintf(sub, sizeof(sub), "%s/%s", day_path, names[i]);
		rmdir(sub);
	}
	if (names)
		free_names(names, n);
	rmdir(day_path);
}

/* Get total size of .mp4 files in a day directory */
static int64_t dir_mp4_size(const char *dir_path)
{
	int n;
	day_file_t *f = collect_day(dir_path, &n);
	int64_t total = 0;
	for (int i = 0; f && i < n; i++) {
		struct stat st;
		if (is_mp4(DF_NAME(&f[i])) && stat(f[i].path, &st) == 0)
			total += st.st_size;
	}
	free(f);
	return total;
}

static bool is_day_dir(const char *n)
{
	return strlen(n) == 10 && n[4] == '-' && n[7] == '-';
}

static bool is_mp4(const char *n)
{
	size_t len = strlen(n);
	return len >= 4 && strcmp(n + len - 4, ".mp4") == 0;
}

/* Bytes to free: over max_storage_mb, or short of min_free_mb */
static int64_t storage_need(rmr_storage_t *st, char **days, int day_count)
{
	int64_t need = 0;
	if (st->max_storage_mb > 0) {
		int64_t total = 0;
		for (int i = 0; i < day_count; i++) {
			if (!is_day_dir(days[i]))
				continue;
			char dir_path[512];
			snprintf(dir_path, sizeof(dir_path), "%s/%s", st->base_path, days[i]);
			total += dir_mp4_size(dir_path);
		}
		need = total - (int64_t)st->max_storage_mb * 1024 * 1024;
	}
	struct statvfs v;
	if (st->min_free_mb > 0 && statvfs(st->base_path, &v) == 0) {
		int64_t short_by =
			(int64_t)st->min_free_mb * 1024 * 1024 - (int64_t)v.f_bavail * v.f_frsize;
		if (short_by > need)
			need = short_by;
	}
	return need;
}

/* Files this process holds open: the segments and clips being written.
 * Matched by inode, so a storage_path spelled differently from the
 * kernel's canonical path (trailing slash, symlink) still matches. */
typedef struct {
	dev_t dev;
	ino_t ino;
} held_file_t;

static int open_files(held_file_t *held, int max)
{
	DIR *d = opendir("/proc/self/fd");
	int n = 0;
	struct dirent *e;
	while (d && n < max && (e = readdir(d)) != NULL) {
		char link[300];
		struct stat fst;
		snprintf(link, sizeof(link), "/proc/self/fd/%s", e->d_name);
		if (stat(link, &fst) == 0 && S_ISREG(fst.st_mode)) {
			held[n].dev = fst.st_dev;
			held[n].ino = fst.st_ino;
			n++;
		}
	}
	if (d)
		closedir(d);
	return n;
}

static bool is_held(const held_file_t *held, int n, const char *path)
{
	struct stat fst;
	if (stat(path, &fst) != 0)
		return false;
	for (int i = 0; i < n; i++)
		if (held[i].dev == fst.st_dev && held[i].ino == fst.st_ino)
			return true;
	return false;
}

/* Loop recording: whole segments go oldest name first, a file with its
 * tagged siblings (X.mp4, X_R.mp4), until the budget holds. Only files
 * this process has open are kept, so a clock that steps (RTC before NTP,
 * a GPS fix) can reorder what goes first but never stops the cleanup or
 * takes a file being written. Clips live elsewhere and are not touched. */
static int enforce_limit_locked(rmr_storage_t *st)
{
	if (!st || (st->max_storage_mb <= 0 && st->min_free_mb <= 0))
		return 0;

	int day_count;
	char **days = scan_dir_sorted(st->base_path, &day_count);
	if (!days)
		return 0;

	int64_t need = storage_need(st, days, day_count);
	held_file_t held[128];
	int held_n = open_files(held, 128);
	int64_t freed = 0;
	int deleted = 0;
	bool kept_open = false;

	for (int i = 0; i < day_count && freed < need; i++) {
		if (!is_day_dir(days[i]))
			continue;
		char dir_path[512];
		snprintf(dir_path, sizeof(dir_path), "%s/%s", st->base_path, days[i]);
		int file_count;
		day_file_t *files = collect_day(dir_path, &file_count);
		if (!files)
			continue;
		int j = 0;
		while (j < file_count && freed < need) {
			const char *first = DF_NAME(&files[j]);
			size_t key = strcspn(first, "_.");
			int g = j;
			bool in_use = false;
			while (g < file_count && strncmp(DF_NAME(&files[g]), first, key) == 0 &&
			       (DF_NAME(&files[g])[key] == '_' || DF_NAME(&files[g])[key] == '.')) {
				if (!in_use)
					in_use = is_held(held, held_n, files[g].path);
				g++;
			}
			if (g == j)
				g = j + 1; /* a name that groups with nothing */
			if (in_use) {
				kept_open = true;
				j = g;
				continue;
			}
			for (int k = j; k < g; k++) {
				if (!is_mp4(DF_NAME(&files[k])))
					continue;
				struct stat fst;
				if (stat(files[k].path, &fst) == 0 && unlink(files[k].path) == 0) {
					freed += fst.st_size;
					deleted++;
					/* a fragmented segment's truncate holds f2fs locks
					 * the writer needs: give it the card between files */
					if (st->pacing)
						usleep(500000);
					if (__atomic_load_n(&st->clean_stop, __ATOMIC_RELAXED))
						need = 0; /* shutting down: the next pass resumes */
				}
			}
			j = g;
		}
		free(files);
		prune_day(dir_path); /* only what is empty */
	}
	free_names(days, day_count);

	if (deleted > 0)
		RSS_INFO("storage cleanup: deleted %d files, %.1f MB", deleted,
			 (double)freed / (1024.0 * 1024.0));
	else if (need > 0 && kept_open)
		RSS_WARN("storage cleanup: %.1f MB over budget, only open files left",
			 (double)need / (1024.0 * 1024.0));

	return deleted;
}

int rmr_storage_enforce_limit(rmr_storage_t *st)
{
	if (!st)
		return 0;
	pthread_mutex_lock(&st->clean_lock);
	int n = enforce_limit_locked(st);
	pthread_mutex_unlock(&st->clean_lock);
	return n;
}

static void *clean_thread(void *arg)
{
	rmr_storage_t *st = arg;
	pthread_mutex_lock(&st->req_lock);
	while (!st->clean_stop) {
		if (!st->clean_pending) {
			pthread_cond_wait(&st->req_cv, &st->req_lock);
			continue;
		}
		st->clean_pending = false;
		if (st->cleanup_delay_ms > 0) {
			/* away from the rotation's burst of closes and opens */
			struct timespec until;
			clock_gettime(CLOCK_REALTIME, &until);
			until.tv_sec += st->cleanup_delay_ms / 1000;
			until.tv_nsec += (long)(st->cleanup_delay_ms % 1000) * 1000000L;
			if (until.tv_nsec >= 1000000000L) {
				until.tv_sec++;
				until.tv_nsec -= 1000000000L;
			}
			while (!st->clean_stop &&
			       pthread_cond_timedwait(&st->req_cv, &st->req_lock, &until) == 0)
				;
			if (st->clean_stop)
				break;
		}
		pthread_mutex_unlock(&st->req_lock);
		pthread_mutex_lock(&st->clean_lock);
		st->pacing = st->cleanup_delay_ms > 0;
		enforce_limit_locked(st);
		st->pacing = false;
		pthread_mutex_unlock(&st->clean_lock);
		pthread_mutex_lock(&st->req_lock);
	}
	pthread_mutex_unlock(&st->req_lock);
	return NULL;
}

void rmr_storage_request_cleanup(rmr_storage_t *st)
{
	if (!rmr_storage_loops(st))
		return;
	pthread_mutex_lock(&st->req_lock);
	if (!st->clean_started && pthread_create(&st->clean_thr, NULL, clean_thread, st) == 0)
		st->clean_started = true;
	st->clean_pending = true;
	pthread_cond_signal(&st->req_cv);
	pthread_mutex_unlock(&st->req_lock);
	if (!st->clean_started)
		rmr_storage_enforce_limit(st); /* no thread: do it here */
}

/*
 * rmr_storage.c -- Recording file rotation and storage management
 *
 * Directory layout: {base_path}/YYYY-MM-DD/HH-MM-SS.mp4
 * One directory per day. Cleanup deletes oldest day directories first.
 */

#include "rmr_storage.h"

#include <rss_common.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#define RMR_TMP_SUFFIX	   ".tmp"
#define RMR_TMP_SUFFIX_LEN (sizeof(RMR_TMP_SUFFIX) - 1)

struct rmr_storage {
	char base_path[256];
	int segment_minutes;
	int segment_seconds;
	int max_storage_mb;
	uint64_t prealloc_bytes;   /* segment reservation, 0 = none        */
	bool refuse_logged;	   /* rootfs auto-create refusal logged once */
	bool swept;		   /* stale .tmp sweep done once            */
	int64_t last_wait_warn_us; /* rate-limit for the waiting log       */
};

rmr_storage_t *rmr_storage_create(const rmr_storage_config_t *cfg)
{
	if (!cfg || !cfg->base_path)
		return NULL;

	rmr_storage_t *st = calloc(1, sizeof(*st));
	if (!st)
		return NULL;

	snprintf(st->base_path, sizeof(st->base_path), "%s", cfg->base_path);
	st->segment_minutes = cfg->segment_minutes > 0 ? cfg->segment_minutes : 5;
	st->segment_seconds = cfg->segment_seconds;
	st->max_storage_mb = cfg->max_storage_mb;
	st->prealloc_bytes = cfg->prealloc_bytes;

	return st;
}

void rmr_storage_destroy(rmr_storage_t *st)
{
	free(st);
}

int rmr_storage_open_segment(rmr_storage_t *st, char *path_out, int path_out_size)
{
	if (!st)
		return -1;

	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);

	/* Create date directory: {base}/YYYY-MM-DD/ */
	char dir[320];
	snprintf(dir, sizeof(dir), "%s/%04d-%02d-%02d", st->base_path, tm.tm_year + 1900,
		 tm.tm_mon + 1, tm.tm_mday);
	rss_mkdir_p(dir);

	/* Final name: HH-MM-SS.mp4. Recording happens under a .tmp twin
	 * of that name; the .mp4 appears only when the segment closes
	 * cleanly (rename), so a power loss mid-segment can never leave
	 * a truncated .mp4 behind for players and NVR importers to trip
	 * over — only the .tmp, which is swept at the next start. */
	snprintf(path_out, path_out_size, "%s/%02d-%02d-%02d.mp4", dir, tm.tm_hour, tm.tm_min,
		 tm.tm_sec);

	char tmp_path[320];
	snprintf(tmp_path, sizeof(tmp_path), "%s" RMR_TMP_SUFFIX, path_out);

	int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		RSS_ERROR("failed to open segment: %s", tmp_path);
		return -1;
	}

	/* Optional reservation. ftruncate, never fallocate: vfat rejects
	 * fallocate (EOPNOTSUPP) and exfat-nofuse does not implement it,
	 * while their truncate-growth path (fat_cont_expand) only writes
	 * metadata — the reserved clusters are never touched, so a later
	 * power loss cannot splice garbage from an old recording into
	 * this one, and a yanked card keeps its old data intact. A
	 * failed reservation (e.g. card nearly full) only logs: the
	 * segment still records, growing organically. */
	if (st->prealloc_bytes > 0 && ftruncate(fd, (off_t)st->prealloc_bytes) != 0)
		RSS_WARN("segment prealloc of %llu bytes failed (%s): growing organically",
			 (unsigned long long)st->prealloc_bytes, strerror(errno));

	return fd;
}

void rmr_storage_close_segment(int fd, const char *path, uint64_t bytes)
{
	if (fd < 0)
		return;

	if (bytes == 0) {
		/* Nothing worth publishing — discard the .tmp. */
		close(fd);
		if (path) {
			char tmp_path[320];
			snprintf(tmp_path, sizeof(tmp_path), "%s" RMR_TMP_SUFFIX, path);
			unlink(tmp_path);
		}
		return;
	}

	/* Shrink the reservation to what was actually written, flush,
	 * then publish under the final name. rename() is atomic; a
	 * crash before it loses only the in-flight segment. */
	if (ftruncate(fd, (off_t)bytes) != 0)
		RSS_ERROR("failed to shrink segment to %llu bytes: %s", (unsigned long long)bytes,
			  strerror(errno));
	fsync(fd);
	close(fd);

	if (path) {
		char tmp_path[320];
		snprintf(tmp_path, sizeof(tmp_path), "%s" RMR_TMP_SUFFIX, path);
		if (rename(tmp_path, path) != 0)
			RSS_ERROR("failed to publish segment %s: %s", path, strerror(errno));
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

/* YYYY-MM-DD only — deliberately not clips/, timelapse/, or anything
 * user-created; each storage instance sweeps its own date dirs. */
static bool is_date_dir(const char *name)
{
	if (strlen(name) != 10 || name[4] != '-' || name[7] != '-')
		return false;
	for (int i = 0; i < 10; i++) {
		if (i == 4 || i == 7)
			continue;
		if (name[i] < '0' || name[i] > '9')
			return false;
	}
	return true;
}

/*
 * Remove .tmp leftovers from an unclean shutdown. rmr is the only
 * writer and this runs before the first segment of the current run
 * opens, so any .tmp found is garbage from a previous run.
 */
static void sweep_stale_tmp(rmr_storage_t *st)
{
	int removed = 0;

	DIR *d = opendir(st->base_path);
	if (!d)
		return;

	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		if (!is_date_dir(ent->d_name))
			continue;

		char day_dir[320];
		snprintf(day_dir, sizeof(day_dir), "%s/%s", st->base_path, ent->d_name);

		DIR *dd = opendir(day_dir);
		if (!dd)
			continue;
		struct dirent *fe;
		while ((fe = readdir(dd)) != NULL) {
			size_t len = strlen(fe->d_name);
			if (len < 5 + RMR_TMP_SUFFIX_LEN + 4)
				continue; /* shorter than HH-MM-SS.mp4.tmp */
			if (strcmp(fe->d_name + len - 4 - RMR_TMP_SUFFIX_LEN,
				   ".mp4" RMR_TMP_SUFFIX) != 0)
				continue;

			char fpath[512];
			snprintf(fpath, sizeof(fpath), "%s/%s", day_dir, fe->d_name);
			if (unlink(fpath) == 0)
				removed++;
		}
		closedir(dd);
	}
	closedir(d);

	if (removed > 0)
		RSS_INFO("removed %d partial recording(s) left by an unclean shutdown", removed);
}

bool rmr_storage_available(rmr_storage_t *st)
{
	if (!st)
		return false;

	bool ok = access(st->base_path, W_OK) == 0 || storage_try_create(st);
	if (!ok) {
		int64_t now = rss_timestamp_us();
		if (now - st->last_wait_warn_us >= 60000000LL) {
			RSS_WARN("waiting for storage: %s", st->base_path);
			st->last_wait_warn_us = now;
		}
		return false;
	}

	/* First time the media is seen this run: clear whatever an
	 * unclean shutdown left behind. rmr is the only writer and no
	 * segment is open yet, so no .tmp can be in flight. */
	if (!st->swept) {
		st->swept = true;
		sweep_stale_tmp(st);
	}
	return true;
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

/* Get total size of .mp4 files in a day directory */
static int64_t dir_mp4_size(const char *dir_path)
{
	DIR *d = opendir(dir_path);
	if (!d)
		return 0;

	int64_t total = 0;
	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		size_t len = strlen(ent->d_name);
		if (len < 4 || strcmp(ent->d_name + len - 4, ".mp4") != 0)
			continue;
		char fpath[768];
		snprintf(fpath, sizeof(fpath), "%s/%s", dir_path, ent->d_name);
		struct stat st;
		if (stat(fpath, &st) == 0)
			total += st.st_size;
	}
	closedir(d);
	return total;
}

int rmr_storage_enforce_limit(rmr_storage_t *st)
{
	if (!st || st->max_storage_mb <= 0)
		return 0;

	int64_t max_bytes = (int64_t)st->max_storage_mb * 1024 * 1024;
	int deleted = 0;

	/* Scan day directories (sorted oldest first) */
	int day_count;
	char **days = scan_dir_sorted(st->base_path, &day_count);
	if (!days)
		return 0;

	/* Calculate total usage */
	int64_t total = 0;
	for (int i = 0; i < day_count; i++) {
		char dir_path[512];
		snprintf(dir_path, sizeof(dir_path), "%s/%s", st->base_path, days[i]);
		total += dir_mp4_size(dir_path);
	}

	/* Delete oldest day directories until under limit */
	for (int i = 0; i < day_count && total > max_bytes; i++) {
		char dir_path[512];
		snprintf(dir_path, sizeof(dir_path), "%s/%s", st->base_path, days[i]);

		/* Delete .mp4 files in this day directory (skip non-mp4) */
		int file_count;
		char **files = scan_dir_sorted(dir_path, &file_count);
		if (files) {
			for (int j = 0; j < file_count; j++) {
				size_t flen = strlen(files[j]);
				if (flen < 4 || strcmp(files[j] + flen - 4, ".mp4") != 0)
					continue;
				char fpath[768];
				snprintf(fpath, sizeof(fpath), "%s/%s", dir_path, files[j]);
				struct stat fst;
				if (stat(fpath, &fst) == 0) {
					total -= fst.st_size;
					deleted++;
				}
				unlink(fpath);
			}
			free_names(files, file_count);
		}

		/* Remove empty directory */
		rmdir(dir_path);
	}

	free_names(days, day_count);

	if (deleted > 0)
		RSS_INFO("storage cleanup: deleted %d files (%.1f MB remaining)", deleted,
			 (double)total / (1024.0 * 1024.0));

	return deleted;
}

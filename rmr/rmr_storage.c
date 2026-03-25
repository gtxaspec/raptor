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
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

struct rmr_storage {
	char base_path[256];
	int segment_minutes;
	int max_storage_mb;
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
	st->max_storage_mb = cfg->max_storage_mb;

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

	/* Filename: HH-MM-SS.mp4 */
	snprintf(path_out, path_out_size, "%s/%02d-%02d-%02d.mp4", dir, tm.tm_hour, tm.tm_min,
		 tm.tm_sec);

	int fd = open(path_out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		RSS_ERROR("failed to open segment: %s", path_out);

	return fd;
}

void rmr_storage_close_segment(int fd)
{
	if (fd >= 0) {
		fsync(fd);
		close(fd);
	}
}

bool rmr_storage_should_rotate(rmr_storage_t *st, int64_t segment_start_us)
{
	if (!st || st->segment_minutes <= 0)
		return false;
	int64_t elapsed = rss_timestamp_us() - segment_start_us;
	return elapsed >= (int64_t)st->segment_minutes * 60 * 1000000LL;
}

bool rmr_storage_available(rmr_storage_t *st)
{
	if (!st)
		return false;
	return access(st->base_path, W_OK) == 0;
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

		/* Delete all .mp4 files in this day directory */
		int file_count;
		char **files = scan_dir_sorted(dir_path, &file_count);
		if (files) {
			for (int j = 0; j < file_count; j++) {
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

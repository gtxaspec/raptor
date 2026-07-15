#include "greatest.h"
#include "../rmr/rmr_storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool same_dev_as_root(const char *path)
{
	struct stat a, b;
	if (stat("/", &a) != 0 || stat(path, &b) != 0)
		return true;
	return a.st_dev == b.st_dev;
}

TEST storage_available_existing_dir(void)
{
	char dir[] = "/tmp/rmr_storage_test_XXXXXX";
	ASSERT(mkdtemp(dir));

	rmr_storage_config_t cfg = {.base_path = dir, .segment_minutes = 5};
	rmr_storage_t *st = rmr_storage_create(&cfg);
	ASSERT(st);
	ASSERT(rmr_storage_available(st));

	rmr_storage_destroy(st);
	rmdir(dir);
	PASS();
}

TEST storage_autocreate_on_mounted_fs(void)
{
	/* /dev/shm is tmpfs on Linux — a different filesystem than "/",
	 * so auto-creation must kick in for the missing directory. */
	if (same_dev_as_root("/dev/shm"))
		SKIP();

	char base[64];
	snprintf(base, sizeof(base), "/dev/shm/rmr_st_%d/sub", getpid());

	rmr_storage_config_t cfg = {.base_path = base, .segment_minutes = 5};
	rmr_storage_t *st = rmr_storage_create(&cfg);
	ASSERT(st);
	ASSERT(rmr_storage_available(st));
	ASSERT_EQ(0, access(base, W_OK));

	rmr_storage_destroy(st);
	rmdir(base);
	*strrchr(base, '/') = '\0';
	rmdir(base);
	PASS();
}

TEST storage_refuses_rootfs_autocreate(void)
{
	/* /var/tmp sits on the root filesystem on typical hosts; skip
	 * when it does not so the test stays environment-independent. */
	if (!same_dev_as_root("/var/tmp"))
		SKIP();

	char base[64];
	snprintf(base, sizeof(base), "/var/tmp/rmr_st_refuse_%d", getpid());

	rmr_storage_config_t cfg = {.base_path = base, .segment_minutes = 5};
	rmr_storage_t *st = rmr_storage_create(&cfg);
	ASSERT(st);
	ASSERT_FALSE(rmr_storage_available(st));
	ASSERT(access(base, F_OK) != 0); /* nothing was created */

	rmr_storage_destroy(st);
	PASS();
}

SUITE(storage_suite)
{
	RUN_TEST(storage_available_existing_dir);
	RUN_TEST(storage_autocreate_on_mounted_fs);
	RUN_TEST(storage_refuses_rootfs_autocreate);
}

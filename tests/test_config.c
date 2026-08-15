/*
 * test_config.c -- rss_config getter/default semantics.
 *
 * Pins the display-only nature of stored defaults: a getter's miss
 * path records its fallback so config-get-section can show resolved
 * values, but that record must never act as configuration -- not for
 * a later reader with a different fallback (rvd cached 1920x1080
 * before the sensor was known and every 720p camera upscaled), not
 * for a presence probe, and not for a save to a fresh file.
 */

#include "greatest.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <rss_common.h>

/* An empty config object via an empty temp file (load returns NULL
 * for a missing path). Caller removes the file. */
static rss_config_t *empty_cfg(char *path, size_t cap)
{
	snprintf(path, cap, "/tmp/rss_cfg_test_%d_%ld.conf", getpid(), (long)random());
	FILE *f = fopen(path, "w");
	if (!f)
		return NULL;
	fclose(f);
	return rss_config_load(path);
}

TEST cfg_default_is_display_only(void)
{
	char path[128];
	rss_config_t *cfg = empty_cfg(path, sizeof(path));
	ASSERT(cfg);

	/* First reader guesses 1920; the later reader knows the sensor
	 * is 720p wide. The first guess must not win. */
	ASSERT_EQ(1920, rss_config_get_int(cfg, "stream0", "width", 1920));
	ASSERT_EQ(1280, rss_config_get_int(cfg, "stream0", "width", 1280));
	ASSERT_EQ(0, rss_config_get_int(cfg, "stream0", "width", 0));

	/* Same for strings and bools. */
	ASSERT_STR_EQ("aac", rss_config_get_str(cfg, "audio", "codec", "aac"));
	ASSERT_STR_EQ("opus", rss_config_get_str(cfg, "audio", "codec", "opus"));
	ASSERT_EQ(true, rss_config_get_bool(cfg, "x", "flag", true));
	ASSERT_EQ(false, rss_config_get_bool(cfg, "x", "flag", false));

	rss_config_free(cfg);
	unlink(path);
	PASS();
}

static void count_key_cb(const char *key, const char *value, void *ud)
{
	char *out = ud;
	if (strcmp(key, "width") == 0)
		snprintf(out, 32, "%s", value);
}

TEST cfg_default_still_visible_to_display(void)
{
	char path[128];
	rss_config_t *cfg = empty_cfg(path, sizeof(path));
	ASSERT(cfg);

	(void)rss_config_get_int(cfg, "stream0", "width", 1920);
	(void)rss_config_get_int(cfg, "stream0", "width", 1280);

	/* config-get-section iterates entries: the display value tracks
	 * the most recent reader (during boot the authoritative reader
	 * runs last, so the display shows what the daemon uses). */
	char seen[32] = "";
	rss_config_foreach(cfg, "stream0", count_key_cb, seen);
	ASSERT_STR_EQ("1280", seen);

	rss_config_free(cfg);
	unlink(path);
	PASS();
}

TEST cfg_file_value_beats_every_default(void)
{
	char path[128];
	snprintf(path, sizeof(path), "/tmp/rss_cfg_test_%d_f.conf", getpid());
	FILE *f = fopen(path, "w");
	ASSERT(f);
	fputs("[stream0]\nwidth = 1280\n", f);
	fclose(f);

	rss_config_t *cfg = rss_config_load(path);
	ASSERT(cfg);
	ASSERT_EQ(1280, rss_config_get_int(cfg, "stream0", "width", 1920));
	ASSERT_EQ(1280, rss_config_get_int(cfg, "stream0", "width", 0));

	rss_config_free(cfg);
	unlink(path);
	PASS();
}

TEST cfg_set_makes_it_real(void)
{
	char path[128];
	rss_config_t *cfg = empty_cfg(path, sizeof(path));
	ASSERT(cfg);

	(void)rss_config_get_int(cfg, "a", "k", 100); /* display-only */
	ASSERT_FALSE(rss_config_has_dirty(cfg));

	rss_config_set_int(cfg, "a", "k", 42);
	ASSERT(rss_config_has_dirty(cfg));
	ASSERT_EQ(42, rss_config_get_int(cfg, "a", "k", 999));

	rss_config_free(cfg);
	unlink(path);
	PASS();
}

TEST cfg_null_probe_is_order_free(void)
{
	char path[128];
	rss_config_t *cfg = empty_cfg(path, sizeof(path));
	ASSERT(cfg);

	/* ric's ir940 probe pattern: "does the config carry the key" --
	 * historically it HAD to run before the get_bool or the stored
	 * default made the key look present forever. Now both orders
	 * answer the same. */
	ASSERT_EQ(NULL, rss_config_get_str(cfg, "ircut", "ir940", NULL));
	(void)rss_config_get_bool(cfg, "ircut", "ir940", false);
	ASSERT_EQ(NULL, rss_config_get_str(cfg, "ircut", "ir940", NULL));

	rss_config_free(cfg);
	unlink(path);
	PASS();
}

TEST cfg_fresh_file_save_skips_defaults(void)
{
	char path[128];
	rss_config_t *cfg = empty_cfg(path, sizeof(path));
	ASSERT(cfg);
	unlink(path); /* force the whole-config write path on save */

	(void)rss_config_get_int(cfg, "stream0", "width", 1920); /* display-only */
	rss_config_set_int(cfg, "rtsp", "port", 8554);		 /* real */
	ASSERT_EQ(0, rss_config_save(cfg, path));
	rss_config_free(cfg);

	rss_config_t *re = rss_config_load(path);
	ASSERT(re);
	/* The guessed width must not have been frozen into the file... */
	ASSERT_EQ(NULL, rss_config_get_str(re, "stream0", "width", NULL));
	/* ...while the real write survived the round trip. */
	ASSERT_EQ(8554, rss_config_get_int(re, "rtsp", "port", 0));

	rss_config_free(re);
	unlink(path);
	PASS();
}

SUITE(config_suite)
{
	RUN_TEST(cfg_default_is_display_only);
	RUN_TEST(cfg_default_still_visible_to_display);
	RUN_TEST(cfg_file_value_beats_every_default);
	RUN_TEST(cfg_set_makes_it_real);
	RUN_TEST(cfg_null_probe_is_order_free);
	RUN_TEST(cfg_fresh_file_save_skips_defaults);
}

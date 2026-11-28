/*
 * test_osd_pool.c -- the OSD pool estimate counts every region rvd will
 * be asked to create, ROD's own detection overlay included.
 */

#include "greatest.h"

#include <stdio.h>
#include <unistd.h>

#include "rvd_osd_pool.h"

#define SUB_OVERLAY(w, h) ((uint32_t)(w) * (uint32_t)(h) * 4)

/* The stock text elements, then whatever the case adds. */
static const char base_conf[] = "[stream0]\nwidth = 1920\nheight = 1080\n"
				"[stream1]\nwidth = 640\nheight = 360\n"
				"[osd.timestamp]\ntemplate = %Y-%m-%d %H:%M:%S\n"
				"[osd.camera]\ntemplate = %hostname\n";

static uint32_t bytes_for(const char *extra)
{
	char path[128];
	snprintf(path, sizeof(path), "/tmp/rvd_osd_pool_test_%d.conf", getpid());
	FILE *f = fopen(path, "w");
	if (!f)
		return 0;
	fputs(base_conf, f);
	fputs(extra, f);
	fclose(f);

	rss_config_t *cfg = rss_config_load(path);
	uint32_t n = cfg ? rvd_osd_pool_bytes(cfg) : 0;
	rss_config_free(cfg);
	unlink(path);
	return n;
}

TEST motion_counts_the_overlay_rod_adds_by_itself(void)
{
	uint32_t off = bytes_for("");
	uint32_t on = bytes_for("[motion]\nenabled = true\n");

	ASSERT(off > 0);
	ASSERT_EQ(off + SUB_OVERLAY(640, 360), on);
	PASS();
}

TEST a_declared_detect_overlay_is_counted_once(void)
{
	uint32_t declared = bytes_for("[osd.detect]\ntype = overlay\n");
	uint32_t with_motion = bytes_for("[osd.detect]\ntype = overlay\n"
					 "[motion]\nenabled = true\n");

	ASSERT_EQ(bytes_for("") + SUB_OVERLAY(640, 360), declared);
	ASSERT_EQ(declared, with_motion);
	PASS();
}

TEST a_detect_section_of_another_type_replaces_the_overlay(void)
{
	/* ROD adds its overlay only when no element is named "detect". */
	uint32_t text = bytes_for("[osd.detect]\ntemplate = x\n");
	uint32_t text_motion = bytes_for("[osd.detect]\ntemplate = x\n"
					 "[motion]\nenabled = true\n");

	ASSERT_EQ(text, text_motion);
	PASS();
}

TEST the_overlay_follows_the_sub_stream(void)
{
	uint32_t off = bytes_for("[stream1]\nwidth = 1280\nheight = 720\n");
	uint32_t on = bytes_for("[stream1]\nwidth = 1280\nheight = 720\n"
				"[motion]\nenabled = true\n");

	ASSERT_EQ(off + SUB_OVERLAY(1280, 720), on);
	PASS();
}

TEST no_sub_stream_no_overlay(void)
{
	uint32_t off = bytes_for("[stream1]\nenabled = false\n");
	uint32_t on = bytes_for("[stream1]\nenabled = false\n[motion]\nenabled = true\n");

	ASSERT_EQ(off, on);
	PASS();
}

SUITE(osd_pool_suite)
{
	RUN_TEST(motion_counts_the_overlay_rod_adds_by_itself);
	RUN_TEST(a_declared_detect_overlay_is_counted_once);
	RUN_TEST(a_detect_section_of_another_type_replaces_the_overlay);
	RUN_TEST(the_overlay_follows_the_sub_stream);
	RUN_TEST(no_sub_stream_no_overlay);
}

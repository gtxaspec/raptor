#include "greatest.h"
#include "../rmr/rmr_streams.h"

#include <string.h>

TEST streams_legacy_default(void)
{
	rmr_stream_spec_t s[RMR_MAX_TRACKS];
	ASSERT_EQ(1, rmr_streams_parse(NULL, 0, s, RMR_MAX_TRACKS));
	ASSERT_STR_EQ("main", s[0].ring);
	ASSERT_STR_EQ("", s[0].tag);
	ASSERT_EQ(1, rmr_streams_parse("", 2, s, RMR_MAX_TRACKS));
	ASSERT_STR_EQ("s1_main", s[0].ring);
	/* out-of-range legacy index falls back to main, as it always did */
	ASSERT_EQ(1, rmr_streams_parse(NULL, 42, s, RMR_MAX_TRACKS));
	ASSERT_STR_EQ("main", s[0].ring);
	PASS();
}

TEST streams_front_and_rear(void)
{
	rmr_stream_spec_t s[RMR_MAX_TRACKS];
	ASSERT_EQ(2, rmr_streams_parse("main, s1_main:R", 0, s, RMR_MAX_TRACKS));
	ASSERT_STR_EQ("main", s[0].ring);
	ASSERT_STR_EQ("", s[0].tag);
	ASSERT_STR_EQ("s1_main", s[1].ring);
	ASSERT_STR_EQ("R", s[1].tag);
	PASS();
}

TEST streams_derived_tags(void)
{
	rmr_stream_spec_t s[RMR_MAX_TRACKS];
	ASSERT_EQ(3, rmr_streams_parse("main,s1_main,s1_sub", 0, s, RMR_MAX_TRACKS));
	ASSERT_STR_EQ("s1", s[1].tag);
	/* s1 is taken: the whole ring name keeps the files apart */
	ASSERT_STR_EQ("s1_sub", s[2].tag);
	ASSERT_EQ(2, rmr_streams_parse("s1_main, main", 0, s, RMR_MAX_TRACKS));
	ASSERT_STR_EQ("", s[0].tag);
	ASSERT_STR_EQ("main", s[1].tag);
	PASS();
}

TEST streams_rejects_bad_entries(void)
{
	rmr_stream_spec_t s[RMR_MAX_TRACKS];
	/* first track never tagged; duplicate ring and tag dropped; a tag
	 * that would escape the file name dropped */
	ASSERT_EQ(2, rmr_streams_parse("main:F, s1_main:R, main, s2_main:R, s1_sub:../x", 0, s,
				       RMR_MAX_TRACKS));
	ASSERT_STR_EQ("", s[0].tag);
	ASSERT_STR_EQ("s1_main", s[1].ring);
	/* surplus entries beyond the track limit */
	ASSERT_EQ(3, rmr_streams_parse("main,s1_main:R,s2_main:L,sub:S", 0, s, RMR_MAX_TRACKS));
	ASSERT_STR_EQ("s2_main", s[2].ring);
	/* nothing usable: the legacy stream */
	ASSERT_EQ(1, rmr_streams_parse(" , ,", 1, s, RMR_MAX_TRACKS));
	ASSERT_STR_EQ("sub", s[0].ring);
	/* a ring name too long for the ring namespace */
	ASSERT_EQ(1, rmr_streams_parse("main, aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa:R", 0, s,
				       RMR_MAX_TRACKS));
	PASS();
}

TEST streams_prebuf_size(void)
{
	/* 8 Mbit/s, 5 s pre-roll: 1 MB/s * 6 s * 1.25 */
	ASSERT_EQ(7500000u, rmr_prebuf_size_for_rate(1000000, 5, 16u << 20));
	/* 16 Mbit/s fits under 16 MB; 24 Mbit/s hits it */
	ASSERT_EQ(15000000u, rmr_prebuf_size_for_rate(2000000, 5, 16u << 20));
	ASSERT_EQ(16u << 20, rmr_prebuf_size_for_rate(3000000, 5, 16u << 20));
	/* small streams keep the 1 MB floor */
	ASSERT_EQ(1u << 20, rmr_prebuf_size_for_rate(10000, 5, 16u << 20));
	/* a ceiling under the floor wins */
	ASSERT_EQ(512u << 10, rmr_prebuf_size_for_rate(1000000, 5, 512u << 10));
	PASS();
}

SUITE(streams_suite)
{
	RUN_TEST(streams_legacy_default);
	RUN_TEST(streams_front_and_rear);
	RUN_TEST(streams_derived_tags);
	RUN_TEST(streams_rejects_bad_entries);
	RUN_TEST(streams_prebuf_size);
}

/*
 * rmr_streams.c -- [recording] streams: which rings rmr records
 */

#include "rmr_streams.h"

#include <rss_common.h>

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static const char *const ring_names[] = {"main",   "sub",   "s1_main",	"s1_sub",  "s2_main",
					 "s2_sub", "jpeg0", "s1_jpeg0", "s2_jpeg0"};

const char *rmr_stream_ring_name(int idx)
{
	if (idx < 0 || idx >= (int)(sizeof(ring_names) / sizeof(ring_names[0])))
		idx = 0;
	return ring_names[idx];
}

/* Tags end up in file names and ring names in /dev/shm paths */
static bool name_ok(const char *s, size_t max)
{
	size_t n = strlen(s);
	if (n == 0 || n >= max)
		return false;
	for (size_t i = 0; i < n; i++)
		if (!isalnum((unsigned char)s[i]) && s[i] != '_' && s[i] != '-')
			return false;
	return true;
}

static bool taken(const rmr_stream_spec_t *out, int n, const char *ring, const char *tag)
{
	for (int i = 0; i < n; i++)
		if ((ring && strcmp(out[i].ring, ring) == 0) ||
		    (tag && strcmp(out[i].tag, tag) == 0))
			return true;
	return false;
}

static char *trim(char *s)
{
	while (isspace((unsigned char)*s))
		s++;
	char *e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = '\0';
	return s;
}

static void add_entry(char *item, rmr_stream_spec_t *out, int *n, int max)
{
	item = trim(item);
	if (*item == '\0')
		return;
	char *tag = strchr(item, ':');
	if (tag) {
		*tag++ = '\0';
		tag = trim(tag);
		item = trim(item);
	}
	if (*n >= max) {
		RSS_WARN("recording streams: at most %d, ignoring %s", max, item);
		return;
	}
	if (!name_ok(item, RMR_RING_NAME_MAX) || taken(out, *n, item, NULL)) {
		RSS_WARN("recording streams: ignoring ring '%s'", item);
		return;
	}

	rmr_stream_spec_t *s = &out[*n];
	if (*n == 0) {
		if (tag && *tag)
			RSS_WARN("recording streams: the first stream is untagged, ignoring '%s'",
				 tag);
		rss_strlcpy(s->ring, item, sizeof(s->ring));
		s->tag[0] = '\0';
		(*n)++;
		return;
	}

	char want[RMR_TAG_MAX];
	if (tag && *tag) {
		if (!name_ok(tag, sizeof(want))) {
			RSS_WARN("recording streams: bad tag '%s' for %s", tag, item);
			return;
		}
		rss_strlcpy(want, tag, sizeof(want));
	} else {
		size_t len = strcspn(item, "_");
		if (len >= sizeof(want))
			len = sizeof(want) - 1;
		memcpy(want, item, len);
		want[len] = '\0';
		if (taken(out, *n, NULL, want))
			rss_strlcpy(want, item, sizeof(want));
	}
	if (taken(out, *n, NULL, want)) {
		RSS_WARN("recording streams: tag '%s' already used, ignoring %s", want, item);
		return;
	}
	rss_strlcpy(s->ring, item, sizeof(s->ring));
	rss_strlcpy(s->tag, want, sizeof(s->tag));
	(*n)++;
}

int rmr_streams_parse(const char *spec, int legacy_idx, rmr_stream_spec_t *out, int max)
{
	int n = 0;
	if (spec) {
		char buf[256];
		rss_strlcpy(buf, spec, sizeof(buf));
		char *save = NULL;
		for (char *item = strtok_r(buf, ",", &save); item;
		     item = strtok_r(NULL, ",", &save))
			add_entry(item, out, &n, max);
	}
	if (n == 0) {
		rss_strlcpy(out[0].ring, rmr_stream_ring_name(legacy_idx), sizeof(out[0].ring));
		out[0].tag[0] = '\0';
		n = 1;
	}
	return n;
}

uint32_t rmr_prebuf_size_for_rate(uint64_t bytes_per_sec, int prebuffer_sec, uint32_t max_bytes)
{
	uint64_t want = bytes_per_sec * (uint64_t)(prebuffer_sec + 1) * 5 / 4;
	if (want < 1024 * 1024)
		want = 1024 * 1024;
	if (want > max_bytes)
		want = max_bytes;
	return (uint32_t)want;
}

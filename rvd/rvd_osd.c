/*
 * rvd_osd.c -- OSD SHM consumer (stub)
 *
 * When ROD is running, it renders text into OSD SHM double-buffers.
 * This module checks for updates and feeds the bitmap data to the
 * HAL OSD regions.
 *
 * Currently a stub -- will be activated in Phase 4 when ROD exists.
 */

#include <stdio.h>

#include "rvd.h"

void rvd_osd_init(rvd_state_t *st)
{
	/* Try to open OSD SHM (non-fatal if ROD isn't running) */
	for (int i = 0; i < st->stream_count; i++) {
		char name[32];
		snprintf(name, sizeof(name), "osd_%d", i);
		st->osd_shm[i] = rss_osd_open(name);
		if (st->osd_shm[i])
			RSS_DEBUG("opened OSD SHM for stream %d", i);
	}
}

void rvd_osd_check(rvd_state_t *st)
{
	for (int i = 0; i < st->stream_count; i++) {
		if (!st->osd_shm[i])
			continue;

		if (!rss_osd_check_dirty(st->osd_shm[i]))
			continue;

		uint32_t w, h;
		const uint8_t *bitmap = rss_osd_get_active_buffer(st->osd_shm[i], &w, &h);
		if (!bitmap)
			continue;

		/* TODO: Feed bitmap to HAL OSD region
		 * RSS_HAL_CALL(st->ops, osd_update_region_data,
		 *              st->hal_ctx, region_handle, bitmap);
		 */

		rss_osd_clear_dirty(st->osd_shm[i]);
	}
}

void rvd_osd_deinit(rvd_state_t *st)
{
	for (int i = 0; i < st->stream_count; i++) {
		if (st->osd_shm[i]) {
			rss_osd_close(st->osd_shm[i]);
			st->osd_shm[i] = NULL;
		}
	}
}

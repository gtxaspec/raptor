#ifndef RVD_OSD_POOL_H
#define RVD_OSD_POOL_H

#include <stdint.h>

#include <rss_common.h>

/* Bytes the OSD regions of this config need on the main and sub
 * streams: the [osd.*] sections, and the overlay ROD adds by itself
 * with [motion] on. Before headroom, override and floor. */
uint32_t rvd_osd_pool_bytes(rss_config_t *cfg);

#endif /* RVD_OSD_POOL_H */

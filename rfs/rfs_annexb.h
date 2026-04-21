/*
 * rfs_annexb.h -- Annex B video file scanner
 *
 * Scans raw H.264/H.265 Annex B files to build a frame index with
 * codec info, resolution, and display-order positions (for B-frame
 * reordering). All parsing is read-only against mmap'd file data.
 */

#ifndef RFS_ANNEXB_H
#define RFS_ANNEXB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RFS_ANNEXB_MAX_FRAMES 65536

typedef struct {
	uint32_t offset;
	uint32_t length;
	uint32_t display_pos;
	uint16_t nal_type;
	uint8_t is_key;
	uint8_t is_bframe;
} rfs_frame_t;

typedef struct {
	int codec;
	uint32_t width;
	uint32_t height;
	uint8_t profile;
	uint8_t level;
	rfs_frame_t *frames;
	uint32_t frame_count;
} rfs_annexb_info_t;

/*
 * Scan an mmap'd Annex B file. Auto-detects codec from NAL headers
 * or file extension (path). Use codec_hint >= 0 to override.
 * Caller must call rfs_annexb_free() when done.
 * Returns 0 on success, -1 on error.
 */
int rfs_annexb_scan(const uint8_t *data, size_t size, const char *path,
		    int codec_hint, rfs_annexb_info_t *info);

void rfs_annexb_free(rfs_annexb_info_t *info);

#endif /* RFS_ANNEXB_H */

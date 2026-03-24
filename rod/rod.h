/*
 * rod.h -- ROD internal shared state
 */

#ifndef ROD_H
#define ROD_H

#include <rss_ipc.h>
#include <rss_common.h>
#include <schrift.h>

#include <stdbool.h>
#include <stdint.h>

#define ROD_MAX_STREAMS	     2
#define ROD_MAX_REGIONS	     4
#define ROD_GLYPH_CACHE_SIZE 128
#define ROD_MAX_TEXT_CHARS   40 /* max chars for pre-allocated width */

/* Region roles — determines position in RVD */
#define ROD_REGION_TIME	  0 /* top-left */
#define ROD_REGION_UPTIME 1 /* top-right */
#define ROD_REGION_TEXT	  2 /* bottom-left */
#define ROD_REGION_LOGO	  3 /* bottom-right */

/* Cached glyph */
typedef struct {
	uint32_t codepoint;
	int width;
	int height;
	int x_offset;	/* left side bearing */
	int y_offset;	/* baseline offset */
	int advance;	/* horizontal advance */
	uint8_t *alpha; /* single-channel bitmap (width * height) */
} rod_glyph_t;

/* Per-region state */
typedef struct {
	int role;
	bool enabled;
	uint32_t width;
	uint32_t height;
	rss_osd_shm_t *shm;
	bool needs_update;
} rod_region_t;

/* Per-stream font context (different sizes per stream) */
typedef struct {
	SFT sft;
	rod_glyph_t glyphs[ROD_GLYPH_CACHE_SIZE];
	int glyph_count;
	int max_text_width;
	int text_height;
	int ascender;
} rod_font_t;

/* Config from [osd] section */
typedef struct {
	bool enabled;
	char font_path[128];
	int font_size;
	uint32_t font_color; /* BGRA packed */
	int font_stroke;

	bool time_enabled;
	char time_format[64];

	bool uptime_enabled;

	bool text_enabled;
	char text_string[128];

	bool logo_enabled;
	char logo_path[256];
	int logo_width;
	int logo_height;
} rod_config_t;

/* Global state */
typedef struct {
	rod_config_t cfg;

	/* Fonts (one per stream — different sizes) */
	rod_font_t fonts[ROD_MAX_STREAMS];
	int stream_count;

	/* Stream dimensions (read from config) */
	int stream_w[ROD_MAX_STREAMS];
	int stream_h[ROD_MAX_STREAMS];

	/* Regions: [stream][role] */
	rod_region_t regions[ROD_MAX_STREAMS][ROD_MAX_REGIONS];

	/* Logo data (loaded once) */
	uint8_t *logo_data;
	uint32_t logo_data_size;
	/* Sub-stream logo */
	uint8_t *logo_sub_data;
	int logo_sub_w;
	int logo_sub_h;

	/* Control */
	rss_ctrl_t *ctrl;
	rss_config_t *config;
	const char *config_path;

	volatile sig_atomic_t *running;
} rod_state_t;

/* rod_render.c */
int rod_render_init(rod_state_t *st, int stream_idx, int font_size);
void rod_render_deinit(rod_state_t *st, int stream_idx);
rod_glyph_t *rod_glyph_lookup(rod_font_t *font, uint32_t codepoint);
void rod_draw_text(rod_state_t *st, int stream_idx, uint8_t *buf, uint32_t buf_w, uint32_t buf_h,
		   const char *text);
int rod_load_logo(const char *path, int expected_w, int expected_h, uint8_t **out_data);

#endif /* ROD_H */

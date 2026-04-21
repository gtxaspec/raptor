/*
 * rod.h -- ROD internal shared state
 *
 * Dynamic element registry: arbitrary named OSD elements with
 * template variable expansion, replacing the fixed 6-slot system.
 */

#ifndef ROD_H
#define ROD_H

#include <rss_ipc.h>
#include <rss_common.h>
#include <schrift.h>

#include <stdbool.h>
#include <stdint.h>

#define ROD_MAX_STREAMS	     6 /* up to 3 sensors x 2 (main+sub) */
#define ROD_MAX_ELEMENTS     16
#define ROD_ELEM_NAME_LEN    32
#define ROD_GLYPH_CACHE_SIZE 95 /* ASCII 0x20-0x7E */
#define ROD_MAX_FONTS	     8	/* font context pool per stream */
#define ROD_MAX_VARS	     16
#define ROD_TMPL_LEN	     256
#define ROD_EXPANDED_LEN     256

typedef enum {
	ROD_ELEM_TEXT,
	ROD_ELEM_IMAGE,
	ROD_ELEM_OVERLAY,
} rod_elem_type_t;

typedef enum {
	ROD_UPDATE_TICK,
	ROD_UPDATE_CHANGE,
} rod_update_mode_t;

/* Cached glyph */
typedef struct {
	uint32_t codepoint;
	int width;
	int height;
	int x_offset;
	int y_offset;
	int advance;
	uint8_t *alpha;
} rod_glyph_t;

/* Per-stream font context */
typedef struct {
	SFT sft;
	rod_glyph_t glyphs[ROD_GLYPH_CACHE_SIZE];
	int glyph_count;
	int max_text_width;
	int text_height;
	int ascender;
	int size; /* requested pixel size (for dedup) */
	int refcount;
} rod_font_t;

/* Per-stream per-element state */
typedef struct {
	rss_osd_shm_t *shm;
	uint32_t width;
	uint32_t height;
	bool needs_update;
	int font_idx;
} rod_elem_stream_t;

/* Dynamic OSD element */
typedef struct {
	char name[ROD_ELEM_NAME_LEN];
	rod_elem_type_t type;
	bool active;
	bool visible;

	char tmpl[ROD_TMPL_LEN];
	char last_expanded[ROD_EXPANDED_LEN];

	int font_size;
	uint32_t color;
	uint32_t stroke_color;
	int stroke_size;
	int align;
	char position[32];
	int max_chars;

	char image_path[256];
	uint8_t *image_data;
	int image_w;
	int image_h;
	uint8_t *image_sub_data;
	int image_sub_w;
	int image_sub_h;

	rod_update_mode_t update_mode;
	bool sub_streams_only;

	rod_elem_stream_t streams[ROD_MAX_STREAMS];
} rod_element_t;

/* Custom template variable */
typedef struct {
	char name[32];
	char value[128];
} rod_var_t;

/* Global OSD settings from [osd] section */
typedef struct {
	bool enabled;
	char font_path[128];
	int font_size;
	uint32_t font_color;
	uint32_t stroke_color;
	int font_stroke;
	char time_format[64];
} rod_config_t;

/* Global state */
typedef struct {
	rod_config_t settings;

	rod_font_t fonts[ROD_MAX_STREAMS][ROD_MAX_FONTS];
	int stream_count;

	int stream_w[ROD_MAX_STREAMS];
	int stream_h[ROD_MAX_STREAMS];

	rod_element_t elements[ROD_MAX_ELEMENTS];
	int elem_count;

	rod_var_t vars[ROD_MAX_VARS];
	int var_count;

	char hostname[64];

	bool detect_enabled;

	rss_ctrl_t *ctrl;
	rss_config_t *cfg;
	const char *config_path;

	volatile sig_atomic_t *running;
} rod_state_t;

/* rod_render.c */
int rod_render_init(rod_state_t *st, int stream_idx, int font_idx, int font_size);
void rod_render_deinit(rod_state_t *st, int stream_idx, int font_idx);
rod_glyph_t *rod_glyph_lookup(rod_font_t *font, uint32_t codepoint);
void rod_draw_text(rod_state_t *st, int stream_idx, int font_idx, uint8_t *buf, uint32_t buf_w,
		   uint32_t buf_h, const char *text, int align, uint32_t color,
		   uint32_t stroke_color, int stroke_size);
int rod_load_logo(const char *path, int expected_w, int expected_h, uint8_t **out_data);
void rod_draw_rect_outline(uint8_t *buf, uint32_t buf_w, uint32_t buf_h, int x0, int y0, int x1,
			   int y1, uint32_t color_bgra, int thickness);

/* rod_main.c -- element registry */
rod_element_t *rod_find_element(rod_state_t *st, const char *name);
int rod_add_element(rod_state_t *st, const char *name, rod_elem_type_t type, const char *tmpl,
		    const char *position, int align, int font_size, int max_chars,
		    rod_update_mode_t update_mode);
void rod_remove_element(rod_state_t *st, const char *name);
int rod_expand_template(rod_state_t *st, const char *tmpl, char *out, int out_size);
int rod_alloc_font(rod_state_t *st, int stream_idx, int font_size);

#endif /* ROD_H */

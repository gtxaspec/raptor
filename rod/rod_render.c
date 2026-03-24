/*
 * rod_render.c -- Font rendering and logo loading
 *
 * Uses libschrift for TrueType glyph rasterization. Glyphs are cached
 * at startup and composited into BGRA bitmaps for OSD SHM transport.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "rod.h"

int rod_render_init(rod_state_t *st, int stream_idx, int font_size)
{
	rod_font_t *f = &st->fonts[stream_idx];

	f->sft.font = sft_loadfile(st->cfg.font_path);
	if (!f->sft.font) {
		RSS_FATAL("failed to load font: %s", st->cfg.font_path);
		return -1;
	}

	f->sft.xScale = font_size;
	f->sft.yScale = font_size;
	f->sft.xOffset = 0;
	f->sft.yOffset = 0;
	f->sft.flags = SFT_DOWNWARD_Y;

	/* Line metrics */
	SFT_LMetrics lm;
	if (sft_lmetrics(&f->sft, &lm) < 0) {
		RSS_FATAL("sft_lmetrics failed");
		sft_freefont(f->sft.font);
		return -1;
	}
	f->ascender = (int)ceil(lm.ascender);
	int descender = (int)ceil(-lm.descender);
	f->text_height = f->ascender + descender;

	/* Add stroke padding to height */
	if (st->cfg.font_stroke > 0)
		f->text_height += st->cfg.font_stroke * 2;

	/* Cache printable ASCII */
	int max_adv = 0;
	f->glyph_count = 0;

	for (uint32_t cp = 0x20; cp <= 0x7E && f->glyph_count < ROD_GLYPH_CACHE_SIZE; cp++) {
		SFT_Glyph gid;
		if (sft_lookup(&f->sft, cp, &gid) < 0)
			continue;

		SFT_GMetrics gm;
		if (sft_gmetrics(&f->sft, gid, &gm) < 0)
			continue;

		int w = gm.minWidth;
		int h = gm.minHeight;
		if (w <= 0 || h <= 0) {
			/* Space or invisible — still need advance */
			rod_glyph_t *g = &f->glyphs[f->glyph_count++];
			g->codepoint = cp;
			g->width = 0;
			g->height = 0;
			g->x_offset = 0;
			g->y_offset = 0;
			g->advance = (int)ceil(gm.advanceWidth);
			g->alpha = NULL;
			if (g->advance > max_adv)
				max_adv = g->advance;
			continue;
		}

		uint8_t *pixels = malloc(w * h);
		if (!pixels)
			continue;

		SFT_Image img = {.pixels = pixels, .width = w, .height = h};
		if (sft_render(&f->sft, gid, img) < 0) {
			free(pixels);
			continue;
		}

		rod_glyph_t *g = &f->glyphs[f->glyph_count++];
		g->codepoint = cp;
		g->width = w;
		g->height = h;
		g->x_offset = (int)floor(gm.leftSideBearing);
		g->y_offset = (int)floor(gm.yOffset);
		g->advance = (int)ceil(gm.advanceWidth);
		g->alpha = pixels;

		if (g->advance > max_adv)
			max_adv = g->advance;
	}

	/* Max text width: enough for longest expected string + stroke padding */
	int pad = st->cfg.font_stroke > 0 ? st->cfg.font_stroke * 2 : 0;
	f->max_text_width = ROD_MAX_TEXT_CHARS * max_adv + pad;

	/* Ensure even dimensions (SDK may require 2-aligned) */
	f->max_text_width = (f->max_text_width + 1) & ~1;
	f->text_height = (f->text_height + 1) & ~1;

	RSS_INFO("font[%d]: %s size=%d, %d glyphs cached, text=%dx%d", stream_idx,
		 st->cfg.font_path, font_size, f->glyph_count, f->max_text_width, f->text_height);
	return 0;
}

void rod_render_deinit(rod_state_t *st, int stream_idx)
{
	rod_font_t *f = &st->fonts[stream_idx];
	for (int i = 0; i < f->glyph_count; i++)
		free(f->glyphs[i].alpha);
	f->glyph_count = 0;
	if (f->sft.font) {
		sft_freefont(f->sft.font);
		f->sft.font = NULL;
	}
}

rod_glyph_t *rod_glyph_lookup(rod_font_t *font, uint32_t codepoint)
{
	for (int i = 0; i < font->glyph_count; i++) {
		if (font->glyphs[i].codepoint == codepoint)
			return &font->glyphs[i];
	}
	return NULL;
}

/*
 * Composite one glyph into BGRA buffer at (dx, dy) with given color.
 */
static void blit_glyph(uint8_t *buf, uint32_t buf_w, uint32_t buf_h, const rod_glyph_t *g, int dx,
		       int dy, uint8_t b, uint8_t gn, uint8_t r)
{
	if (!g->alpha)
		return;

	for (int gy = 0; gy < g->height; gy++) {
		int py = dy + gy;
		if (py < 0 || py >= (int)buf_h)
			continue;
		for (int gx = 0; gx < g->width; gx++) {
			int px = dx + gx;
			if (px < 0 || px >= (int)buf_w)
				continue;
			uint8_t a = g->alpha[gy * g->width + gx];
			if (a == 0)
				continue;
			uint8_t *dst = buf + (py * buf_w + px) * 4;
			/* Alpha blend over existing content */
			uint8_t inv = 255 - a;
			dst[0] = (uint8_t)((b * a + dst[0] * inv) / 255);
			dst[1] = (uint8_t)((gn * a + dst[1] * inv) / 255);
			dst[2] = (uint8_t)((r * a + dst[2] * inv) / 255);
			dst[3] = (uint8_t)(a + (dst[3] * inv) / 255);
		}
	}
}

/*
 * Draw a text string at pen position, advancing per glyph.
 * Returns final pen_x (for measuring).
 */
static int draw_string(rod_font_t *font, uint8_t *buf, uint32_t buf_w, uint32_t buf_h, int pen_x,
		       int baseline, const char *text, uint8_t b, uint8_t g, uint8_t r)
{
	while (*text) {
		rod_glyph_t *gl = rod_glyph_lookup(font, (uint32_t)(uint8_t)*text);
		if (gl) {
			int dx = pen_x + gl->x_offset;
			int dy = baseline + gl->y_offset;
			blit_glyph(buf, buf_w, buf_h, gl, dx, dy, b, g, r);
			pen_x += gl->advance;
		}
		text++;
	}
	return pen_x;
}

void rod_draw_text(rod_state_t *st, int stream_idx, uint8_t *buf, uint32_t buf_w, uint32_t buf_h,
		   const char *text)
{
	rod_font_t *f = &st->fonts[stream_idx];
	int stroke = st->cfg.font_stroke;
	int baseline = f->ascender + (stroke > 0 ? stroke : 0);

	/* Clear buffer to transparent */
	memset(buf, 0, buf_w * buf_h * 4);

	/* Extract BGRA color components */
	uint32_t c = st->cfg.font_color;
	uint8_t txt_b = (uint8_t)(c & 0xFF);
	uint8_t txt_g = (uint8_t)((c >> 8) & 0xFF);
	uint8_t txt_r = (uint8_t)((c >> 16) & 0xFF);

	int pen_x = stroke > 0 ? stroke : 0;

	if (stroke > 0) {
		/* Stroke: render text at 4 offsets in black */
		for (int sy = -stroke; sy <= stroke; sy++) {
			for (int sx = -stroke; sx <= stroke; sx++) {
				if (sx == 0 && sy == 0)
					continue;
				/* Skip corners for round-ish stroke */
				if (abs(sx) + abs(sy) > stroke)
					continue;
				draw_string(f, buf, buf_w, buf_h, pen_x + sx, baseline + sy, text,
					    0, 0, 0);
			}
		}
	}

	/* Main text on top */
	draw_string(f, buf, buf_w, buf_h, pen_x, baseline, text, txt_b, txt_g, txt_r);
}

int rod_load_logo(const char *path, int expected_w, int expected_h, uint8_t **out_data)
{
	int file_size;
	char *data = rss_read_file(path, &file_size);
	if (!data) {
		RSS_WARN("logo file not found: %s", path);
		return -1;
	}

	uint32_t expected_size = (uint32_t)(expected_w * expected_h * 4);
	if ((uint32_t)file_size != expected_size) {
		RSS_WARN("logo size mismatch: %s is %d bytes, expected %u (%dx%d BGRA)", path,
			 file_size, expected_size, expected_w, expected_h);
		free(data);
		return -1;
	}

	*out_data = (uint8_t *)data;
	return 0;
}

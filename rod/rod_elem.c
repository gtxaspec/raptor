/*
 * rod_elem.c -- Element registry, font management, SHM lifecycle
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rod.h"

void sanitize_text(char *s)
{
	for (int i = 0; s[i]; i++) {
		unsigned char c = (unsigned char)s[i];
		if (c < 0x20)
			s[i] = ' ';
		if (c > 0x7E)
			s[i] = '?';
	}
}

uint32_t parse_color(const char *s)
{
	if (!s)
		return 0xFFFFFFFF;
	return (uint32_t)strtoul(s, NULL, 0);
}

/* ── Element registry ── */

rod_element_t *rod_find_element(rod_state_t *st, const char *name)
{
	for (int i = 0; i < st->elem_count; i++) {
		if (st->elements[i].active && strcmp(st->elements[i].name, name) == 0)
			return &st->elements[i];
	}
	return NULL;
}

int rod_alloc_font(rod_state_t *st, int stream_idx, int font_size)
{
	/* Reuse existing font context with same size */
	for (int i = 0; i < ROD_MAX_FONTS; i++) {
		if (st->fonts[stream_idx][i].refcount > 0 &&
		    st->fonts[stream_idx][i].size == font_size) {
			st->fonts[stream_idx][i].refcount++;
			return i;
		}
	}
	/* Allocate new slot */
	for (int i = 0; i < ROD_MAX_FONTS; i++) {
		if (st->fonts[stream_idx][i].refcount == 0) {
			if (rod_render_init(st, stream_idx, i, font_size) < 0)
				return -1;
			st->fonts[stream_idx][i].size = font_size;
			st->fonts[stream_idx][i].refcount = 1;
			return i;
		}
	}
	RSS_ERROR("font pool exhausted for stream %d", stream_idx);
	return -1;
}

void release_font(rod_state_t *st, int stream_idx, int font_idx)
{
	if (font_idx < 0 || font_idx >= ROD_MAX_FONTS)
		return;
	rod_font_t *f = &st->fonts[stream_idx][font_idx];
	if (f->refcount > 0)
		f->refcount--;
	if (f->refcount == 0)
		rod_render_deinit(st, stream_idx, font_idx);
}

int rod_add_element(rod_state_t *st, const char *name, rod_elem_type_t type, const char *tmpl,
		    const char *position, int align, int font_size, int max_chars,
		    rod_update_mode_t update_mode)
{
	if (rod_find_element(st, name))
		return -1;

	rod_element_t *e = NULL;
	for (int i = 0; i < st->elem_count; i++) {
		if (!st->elements[i].active) {
			e = &st->elements[i];
			break;
		}
	}
	if (!e) {
		if (st->elem_count >= ROD_MAX_ELEMENTS)
			return -1;
		e = &st->elements[st->elem_count++];
	}
	memset(e, 0, sizeof(*e));
	rss_strlcpy(e->name, name, sizeof(e->name));
	e->type = type;
	e->active = true;
	e->visible = true;
	if (tmpl)
		rss_strlcpy(e->tmpl, tmpl, sizeof(e->tmpl));
	if (position)
		rss_strlcpy(e->position, position, sizeof(e->position));
	e->align = align;
	e->font_size = font_size;
	e->stroke_size = -1;
	e->max_chars = max_chars > 0 ? max_chars : 20;
	if (e->max_chars > 128)
		e->max_chars = 128;
	e->update_mode = update_mode;
	for (int s = 0; s < ROD_MAX_STREAMS; s++)
		e->streams[s].font_idx = -1;

	return 0;
}

void rod_remove_element(rod_state_t *st, const char *name)
{
	for (int i = 0; i < st->elem_count; i++) {
		if (!st->elements[i].active || strcmp(st->elements[i].name, name) != 0)
			continue;

		rod_element_t *e = &st->elements[i];
		for (int s = 0; s < st->stream_count; s++) {
			if (e->streams[s].shm) {
				rss_osd_destroy(e->streams[s].shm);
				e->streams[s].shm = NULL;
			}
			release_font(st, s, e->streams[s].font_idx);
			e->streams[s].font_idx = -1;
		}
		free(e->image_data);
		free(e->image_sub_data);
		e->active = false;
		return;
	}
}

/* ── Element state helpers ── */

void mark_all_dirty(rod_state_t *st)
{
	for (int i = 0; i < st->elem_count; i++) {
		if (!st->elements[i].active)
			continue;
		for (int s = 0; s < st->stream_count; s++)
			st->elements[i].streams[s].needs_update = true;
	}
}

void mark_element_dirty(rod_element_t *e, int stream_count)
{
	for (int s = 0; s < stream_count; s++)
		e->streams[s].needs_update = true;
}

/* ── SHM creation ── */

static void font_region_dims(rod_font_t *f, int chars, int stroke, uint32_t *w, uint32_t *h)
{
	int adv = f->max_text_width / 24;
	if (adv < 10)
		adv = 10;
	int pad = stroke > 0 ? stroke * 2 : 0;
	*w = chars * adv + pad;
	*h = f->text_height;
}

void create_elem_shm(rod_state_t *st, rod_element_t *e, int s)
{
	char name[64];
	snprintf(name, sizeof(name), "osd_%d_%s", s, e->name);

	uint32_t w, h;

	if (e->type == ROD_ELEM_TEXT) {
		if (e->streams[s].font_idx < 0)
			return;
		int stroke = e->stroke_size >= 0 ? e->stroke_size : st->settings.font_stroke;
		font_region_dims(&st->fonts[s][e->streams[s].font_idx], e->max_chars, stroke, &w,
				 &h);
	} else if (e->type == ROD_ELEM_IMAGE) {
		if (s == 0) {
			w = e->image_w;
			h = e->image_h;
		} else {
			w = e->image_sub_w > 0 ? e->image_sub_w : e->image_w;
			h = e->image_sub_h > 0 ? e->image_sub_h : e->image_h;
		}
		if (w == 0 || h == 0)
			return;
	} else if (e->type == ROD_ELEM_RECEIPT) {
		if (e->streams[s].font_idx < 0)
			return;
		rod_font_t *f = &st->fonts[s][e->streams[s].font_idx];
		int adv = f->max_text_width / 24;
		if (adv < 10)
			adv = 10;
		int max_ll = e->receipt.max_line_len > 0 ? e->receipt.max_line_len : 80;
		w = max_ll * adv;
		int max_ln = e->receipt.max_lines > 0 ? e->receipt.max_lines : 20;
		h = max_ln * f->text_height;
	} else if (e->type == ROD_ELEM_OVERLAY) {
		w = st->stream_w[s];
		h = st->stream_h[s];
	} else {
		return;
	}

	w = (w + 1) & ~1u;
	h = (h + 1) & ~1u;

	rss_osd_shm_t *shm = rss_osd_create(name, w, h);
	if (!shm) {
		RSS_WARN("failed to create OSD SHM: %s", name);
		return;
	}

	e->streams[s].shm = shm;
	e->streams[s].width = w;
	e->streams[s].height = h;
	e->streams[s].needs_update = true;

	RSS_INFO("osd shm %s: %ux%u", name, w, h);
}

void create_all_shms(rod_state_t *st)
{
	for (int i = 0; i < st->elem_count; i++) {
		rod_element_t *e = &st->elements[i];
		if (!e->active)
			continue;

		for (int s = 0; s < st->stream_count; s++) {
			if (e->sub_streams_only && s == 0)
				continue;
			create_elem_shm(st, e, s);
		}
	}
}

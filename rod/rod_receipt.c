/*
 * rod_receipt.c -- Receipt/POS text overlay element
 *
 * Scrolling text block fed by UART, TCP, FIFO, or control socket.
 * Lines accumulate in a circular buffer and render as a monospaced
 * text block with optional semi-transparent background.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rod.h"

void rod_receipt_add_line(rod_element_t *e, const char *line)
{
	char *dst = e->receipt.lines[e->receipt.head];
	rss_strlcpy(dst, line, ROD_RECEIPT_MAX_LINE + 1);

	/* Sanitize */
	for (int i = 0; dst[i]; i++) {
		unsigned char c = (unsigned char)dst[i];
		if (c < 0x20)
			dst[i] = ' ';
		if (c > 0x7E)
			dst[i] = '?';
	}

	e->receipt.head = (e->receipt.head + 1) % ROD_RECEIPT_MAX_LINES;
	if (e->receipt.count < e->receipt.max_lines)
		e->receipt.count++;
	e->receipt.dirty = true;
}

void rod_receipt_clear(rod_element_t *e)
{
	e->receipt.head = 0;
	e->receipt.count = 0;
	e->receipt.dirty = true;
}

static void emit_accum_line(rod_element_t *e)
{
	if (e->receipt.accum_pos == 0)
		return;
	e->receipt.accum[e->receipt.accum_pos] = '\0';
	rod_receipt_add_line(e, e->receipt.accum);
	e->receipt.accum_pos = 0;
}

void rod_receipt_feed_bytes(rod_state_t *st, rod_element_t *e, const char *data, int len)
{
	int max_len = e->receipt.max_line_len;
	if (max_len <= 0)
		max_len = ROD_RECEIPT_MAX_LINE;

	for (int i = 0; i < len; i++) {
		char c = data[i];
		e->receipt.accum_last_byte_ts = rss_timestamp_us();

		if (c == '\n') {
			emit_accum_line(e);
			continue;
		}
		if (c == '\r')
			continue;

		if (e->receipt.accum_pos < max_len)
			e->receipt.accum[e->receipt.accum_pos++] = c;

		if (e->receipt.accum_pos >= max_len)
			emit_accum_line(e);
	}
	(void)st;
}

/*
 * Render the receipt text block into a SHM buffer.
 * Draws oldest line at top, newest at bottom.
 * Optional semi-transparent background behind text.
 */
void rod_render_receipt(rod_state_t *st, rod_element_t *e, int s)
{
	rod_elem_stream_t *es = &e->streams[s];
	if (!es->shm || es->font_idx < 0)
		return;

	uint8_t *buf = rss_osd_get_draw_buffer(es->shm);
	if (!buf)
		return;

	uint32_t bw = es->width;
	uint32_t bh = es->height;
	rod_font_t *f = &st->fonts[s][es->font_idx];

	/* Empty receipt = fully transparent */
	if (e->receipt.count == 0) {
		memset(buf, 0, bw * bh * 4);
		rss_osd_publish(es->shm);
		e->receipt.dirty = false;
		return;
	}

	/* Clear to transparent, then fill background only for active lines */
	memset(buf, 0, bw * bh * 4);

	uint32_t col = e->has_color ? e->color : st->settings.font_color;
	uint32_t scol = e->has_stroke_color ? e->stroke_color : st->settings.stroke_color;
	int ssz = e->stroke_size >= 0 ? e->stroke_size : st->settings.font_stroke;

	int line_h = f->text_height;
	if (line_h <= 0)
		line_h = 16;

	uint32_t bg = e->receipt.bg_color;
	if (bg) {
		uint32_t fill_h = (uint32_t)(e->receipt.count * line_h);
		if (fill_h > bh)
			fill_h = bh;
		uint8_t *p = buf;
		for (uint32_t i = 0; i < bw * fill_h; i++) {
			p[0] = (uint8_t)(bg & 0xFF);
			p[1] = (uint8_t)((bg >> 8) & 0xFF);
			p[2] = (uint8_t)((bg >> 16) & 0xFF);
			p[3] = (uint8_t)((bg >> 24) & 0xFF);
			p += 4;
		}
	}

	/* Draw lines from oldest to newest */
	int start = (e->receipt.head - e->receipt.count + ROD_RECEIPT_MAX_LINES) %
		    ROD_RECEIPT_MAX_LINES;
	int y_offset = 0;

	for (int i = 0; i < e->receipt.count && y_offset + line_h <= (int)bh; i++) {
		int idx = (start + i) % ROD_RECEIPT_MAX_LINES;
		const char *line = e->receipt.lines[idx];
		if (!line[0])
			continue;

		/* Render into a temporary row region within the buffer.
		 * rod_draw_text clears its target, so we render line-by-line
		 * into offset slices of the main buffer. */
		uint8_t *row_buf = buf + y_offset * bw * 4;
		int row_h = line_h;
		if (y_offset + row_h > (int)bh)
			row_h = (int)bh - y_offset;

		/* Use rod_draw_text's rendering but we need to avoid its memset
		 * clearing the entire region. Instead, draw text directly using
		 * the draw_string path via rod_draw_text on a line-sized slice. */
		rod_draw_text(st, s, es->font_idx, row_buf, bw, row_h, line, e->align, col, scol,
			      ssz);

		y_offset += line_h;
	}

	rss_osd_publish(es->shm);
}

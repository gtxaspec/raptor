/*
 * rod_main.c -- Raptor OSD Daemon
 *
 * Dynamic element registry: renders named OSD elements into BGRA
 * bitmaps via SHM double-buffers for RVD consumption.
 *
 * ROD has no HAL dependency -- it is purely a bitmap producer.
 * RVD handles OSD group/region creation and hardware updates.
 *
 * Elements are defined via [osd.*] config sections and can be
 * added/removed at runtime via control socket commands.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>

#include "rod.h"

/* ── Rendering ── */

static void render_text_element(rod_state_t *st, rod_element_t *e, int s, const char *text)
{
	rod_elem_stream_t *es = &e->streams[s];
	if (!es->shm || es->font_idx < 0)
		return;

	uint8_t *buf = rss_osd_get_draw_buffer(es->shm);
	if (!buf)
		return;

	uint32_t col = e->has_color ? e->color : st->settings.font_color;
	uint32_t scol = e->has_stroke_color ? e->stroke_color : st->settings.stroke_color;
	int ssz = e->stroke_size >= 0 ? e->stroke_size : st->settings.font_stroke;

	rod_draw_text(st, s, es->font_idx, buf, es->width, es->height, text, e->align, col, scol,
		      ssz);
	rss_osd_publish(es->shm);
}

static void render_image_element(rod_state_t *st, rod_element_t *e, int s)
{
	(void)st;
	rod_elem_stream_t *es = &e->streams[s];
	if (!es->shm)
		return;

	uint8_t *img = (s == 0) ? e->image_data : e->image_sub_data;
	int img_w = (s == 0) ? e->image_w : e->image_sub_w;
	int img_h = (s == 0) ? e->image_h : e->image_sub_h;
	if (!img)
		return;

	uint8_t *buf = rss_osd_get_draw_buffer(es->shm);
	if (!buf)
		return;

	memset(buf, 0, es->width * es->height * 4);
	int copy_w = img_w < (int)es->width ? img_w : (int)es->width;
	for (int row = 0; row < img_h && row < (int)es->height; row++)
		memcpy(buf + row * es->width * 4, img + row * img_w * 4, copy_w * 4);

	rss_osd_publish(es->shm);
	es->needs_update = false;
}

static void render_detections(rod_state_t *st, rod_element_t *e)
{
	char resp[2048];
	int ret = rss_ctrl_send_command(RSS_RUN_DIR "/rvd.sock", "{\"cmd\":\"ivs-detections\"}",
					resp, sizeof(resp), 500);
	if (ret < 0)
		return;

	int count = 0;
	rss_json_get_int(resp, "count", &count);
	if (count < 0)
		count = 0;
	if (count > 20)
		count = 20;

	for (int s = 0; s < st->stream_count; s++) {
		if (e->sub_streams_only && s == 0)
			continue;
		rod_elem_stream_t *es = &e->streams[s];
		if (!es->shm)
			continue;

		uint8_t *buf = rss_osd_get_draw_buffer(es->shm);
		if (!buf)
			continue;

		memset(buf, 0, es->width * es->height * 4);

		if (count > 0) {
			cJSON *root = cJSON_Parse(resp);
			if (!root)
				goto publish;
			cJSON *dets = cJSON_GetObjectItem(root, "detections");
			cJSON *det;
			cJSON_ArrayForEach(det, dets)
			{
				int x0 = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(det, "x0"));
				int y0 = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(det, "y0"));
				int x1 = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(det, "x1"));
				int y1 = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(det, "y1"));
				rod_draw_rect_outline(buf, es->width, es->height, x0, y0, x1, y1,
						      0xFF00FF00, 2);
			}
			cJSON_Delete(root);
		}
	publish:
		rss_osd_publish(es->shm);
	}
}

static void render_tick(rod_state_t *st)
{
	if (st->paused)
		return;

	for (int i = 0; i < st->elem_count; i++) {
		rod_element_t *e = &st->elements[i];
		if (!e->active || !e->visible)
			continue;

		if (e->type == ROD_ELEM_TEXT) {
			char expanded[ROD_EXPANDED_LEN];
			rod_expand_template(st, e->tmpl, expanded, sizeof(expanded));

			bool changed = strcmp(expanded, e->last_expanded) != 0;
			if (changed)
				rss_strlcpy(e->last_expanded, expanded, sizeof(e->last_expanded));

			bool should_render = (e->update_mode == ROD_UPDATE_TICK) ||
					     (changed && e->update_mode == ROD_UPDATE_CHANGE);

			if (!should_render) {
				for (int s = 0; s < st->stream_count; s++) {
					if (e->streams[s].needs_update && e->streams[s].shm) {
						render_text_element(st, e, s, expanded);
						e->streams[s].needs_update = false;
					}
				}
				continue;
			}

			for (int s = 0; s < st->stream_count; s++) {
				if (e->sub_streams_only && s == 0)
					continue;
				render_text_element(st, e, s, expanded);
				e->streams[s].needs_update = false;
			}
		} else if (e->type == ROD_ELEM_IMAGE) {
			for (int s = 0; s < st->stream_count; s++) {
				if (e->sub_streams_only && s == 0)
					continue;
				if (e->streams[s].needs_update)
					render_image_element(st, e, s);
			}
		} else if (e->type == ROD_ELEM_RECEIPT) {
			if (e->receipt.accum_timeout > 0 && e->receipt.accum_pos > 0 &&
			    e->receipt.accum_last_byte_ts > 0) {
				int64_t now = rss_timestamp_us();
				int64_t timeout_us = (int64_t)e->receipt.accum_timeout * 1000000;
				if (now - e->receipt.accum_last_byte_ts >= timeout_us) {
					e->receipt.accum[e->receipt.accum_pos] = '\0';
					rod_receipt_add_line(e, e->receipt.accum);
					e->receipt.accum_pos = 0;
				}
			}
			if (e->receipt.dirty) {
				for (int s = 0; s < st->stream_count; s++) {
					if (e->sub_streams_only && s == 0)
						continue;
					rod_render_receipt(st, e, s);
				}
				e->receipt.dirty = false;
			}
		}
	}

	for (int s = 0; s < st->stream_count; s++) {
		for (int i = 0; i < st->elem_count; i++) {
			rod_element_t *e = &st->elements[i];
			if (e->active && e->streams[s].shm) {
				rss_osd_heartbeat(e->streams[s].shm);
				break;
			}
		}
	}
}

/* ── Entry point ── */

static void reopen_receipt_input(rod_element_t *e, int epoll_fd)
{
	if (!e->receipt.input_path[0])
		return;
	int nfd = open(e->receipt.input_path, O_RDONLY | O_NONBLOCK);
	if (nfd < 0)
		return;
	e->receipt.input_fd = nfd;
	if (epoll_fd >= 0) {
		struct epoll_event ev = {.events = EPOLLIN, .data.fd = nfd};
		epoll_ctl(epoll_fd, EPOLL_CTL_ADD, nfd, &ev);
	}
}

int main(int argc, char **argv)
{
	rss_daemon_ctx_t ctx;
	int ret = rss_daemon_init(&ctx, "rod", argc, argv, NULL);
	if (ret != 0)
		return ret < 0 ? 1 : 0;
	if (!rss_config_get_bool(ctx.cfg, "osd", "enabled", true)) {
		RSS_INFO("OSD disabled in config, exiting");
		rss_config_free(ctx.cfg);
		rss_daemon_cleanup("rod");
		return 0;
	}

	rod_state_t st = {0};
	st.cfg = ctx.cfg;
	st.config_path = ctx.config_path;
	st.running = ctx.running;
	load_config(&st);
	init_elements_from_config(&st);

	/* Init fonts for text and receipt elements */
	for (int i = 0; i < st.elem_count; i++) {
		rod_element_t *e = &st.elements[i];
		if (!e->active || (e->type != ROD_ELEM_TEXT && e->type != ROD_ELEM_RECEIPT))
			continue;

		for (int s = 0; s < st.stream_count; s++) {
			int fs = e->font_size > 0 ? e->font_size : st.settings.font_size;
			if (s > 0 && st.stream_h[0] > 0) {
				fs = fs * st.stream_h[s] / st.stream_h[0];
				if (fs < 12)
					fs = 12;
			}
			int fi = rod_alloc_font(&st, s, fs);
			if (fi < 0) {
				RSS_FATAL("font init failed for stream %d element %s", s, e->name);
				goto cleanup;
			}
			e->streams[s].font_idx = fi;
		}
	}

	/* Load logo images */
	for (int i = 0; i < st.elem_count; i++) {
		rod_element_t *e = &st.elements[i];
		if (!e->active || e->type != ROD_ELEM_IMAGE)
			continue;

		if (e->image_path[0] && e->image_w > 0 && e->image_h > 0) {
			if (rod_load_logo(e->image_path, e->image_w, e->image_h, &e->image_data) ==
			    0) {
				RSS_DEBUG("logo loaded: %s (%dx%d)", e->image_path, e->image_w,
					  e->image_h);
			}
		}

		if (st.stream_count > 1) {
			e->image_sub_w = 100;
			e->image_sub_h = 30;
			if (rod_load_logo("/usr/share/images/thingino_100x30.bgra", e->image_sub_w,
					  e->image_sub_h, &e->image_sub_data) != 0) {
				e->image_sub_w = 0;
				e->image_sub_h = 0;
			}
		}
	}

	create_all_shms(&st);

	/* Push all element positions to RVD */
	for (int i = 0; i < st.elem_count; i++) {
		rod_element_t *e = &st.elements[i];
		if (!e->active || !e->position[0])
			continue;
		for (int s = 0; s < st.stream_count; s++) {
			char fwd[128];
			cJSON *j = cJSON_CreateObject();
			if (!j)
				continue;
			cJSON_AddStringToObject(j, "cmd", "osd-position");
			cJSON_AddNumberToObject(j, "stream", s);
			cJSON_AddStringToObject(j, "region", e->name);
			cJSON_AddStringToObject(j, "pos", e->position);
			cJSON_PrintPreallocated(j, fwd, sizeof(fwd), 0);
			cJSON_Delete(j);
			char rvd_resp[256];
			rss_ctrl_send_command(RSS_RUN_DIR "/rvd.sock", fwd, rvd_resp,
					      sizeof(rvd_resp), 1000);
		}
	}

	rss_mkdir_p(RSS_RUN_DIR);
	st.ctrl = rss_ctrl_listen(RSS_RUN_DIR "/rod.sock");

	int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	int ctrl_fd = st.ctrl ? rss_ctrl_get_fd(st.ctrl) : -1;
	if (epoll_fd >= 0 && ctrl_fd >= 0) {
		struct epoll_event ev = {.events = EPOLLIN, .data.fd = ctrl_fd};
		if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, ctrl_fd, &ev) < 0)
			RSS_ERROR("epoll_ctl add ctrl_fd: %s", strerror(errno));
	}

	for (int i = 0; i < st.elem_count; i++) {
		rod_element_t *e = &st.elements[i];
		if (e->active && e->type == ROD_ELEM_RECEIPT && e->receipt.input_fd >= 0 &&
		    epoll_fd >= 0) {
			struct epoll_event ev = {.events = EPOLLIN, .data.fd = e->receipt.input_fd};
			if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, e->receipt.input_fd, &ev) < 0)
				RSS_ERROR("epoll_ctl add receipt fd: %s", strerror(errno));
		}
	}

	render_tick(&st);

	int64_t last_tick = rss_timestamp_us();
	int64_t last_detect = 0;

	while (*st.running) {
		int64_t now = rss_timestamp_us();
		if (now - last_tick >= 1000000) {
			render_tick(&st);
			last_tick = now;
		}

		if (st.detect_enabled && now - last_detect >= 500000) {
			rod_element_t *det = rod_find_element(&st, "detect");
			if (det && det->active && det->visible)
				render_detections(&st, det);
			last_detect = now;
		}

		if (epoll_fd >= 0) {
			struct epoll_event evs[4];
			int n = epoll_wait(epoll_fd, evs, 4, 100);
			for (int ei = 0; ei < n; ei++) {
				int fd = evs[ei].data.fd;
				if (fd == ctrl_fd && st.ctrl) {
					rss_ctrl_accept_and_handle(st.ctrl, rod_ctrl_handler, &st);
				} else {
					for (int ri = 0; ri < st.elem_count; ri++) {
						rod_element_t *re = &st.elements[ri];
						if (re->active && re->type == ROD_ELEM_RECEIPT &&
						    re->receipt.input_fd == fd) {
							char rbuf[256];
							int rn = read(fd, rbuf, sizeof(rbuf));
							if (rn > 0) {
								rod_receipt_feed_bytes(&st, re,
										       rbuf, rn);
							} else if (rn == 0) {
								epoll_ctl(epoll_fd, EPOLL_CTL_DEL,
									  fd, NULL);
								close(fd);
								re->receipt.input_fd = -1;
								reopen_receipt_input(re, epoll_fd);
							}
							break;
						}
					}
				}
			}
		} else {
			usleep(100000);
		}
	}

	RSS_INFO("rod shutting down");

	if (epoll_fd >= 0)
		close(epoll_fd);
	if (st.ctrl)
		rss_ctrl_destroy(st.ctrl);

	for (int i = 0; i < st.elem_count; i++) {
		rod_element_t *e = &st.elements[i];
		if (!e->active)
			continue;
		for (int s = 0; s < st.stream_count; s++) {
			if (e->streams[s].shm)
				rss_osd_destroy(e->streams[s].shm);
		}
		free(e->image_data);
		free(e->image_sub_data);
		if (e->receipt.input_fd >= 0)
			close(e->receipt.input_fd);
	}

cleanup:
	for (int s = 0; s < st.stream_count; s++) {
		for (int fi = 0; fi < ROD_MAX_FONTS; fi++) {
			if (st.fonts[s][fi].refcount > 0)
				rod_render_deinit(&st, s, fi);
		}
	}

	rss_config_free(ctx.cfg);
	rss_daemon_cleanup("rod");

	return 0;
}

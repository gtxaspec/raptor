/*
 * rfs_annexb.c -- Annex B video file scanner
 *
 * Scans raw H.264/H.265 Annex B bitstreams to build a frame index.
 * Each frame is one access unit: preceding non-VCL NALs (SPS/PPS/
 * VPS/SEI/AUD) grouped with the next VCL NAL (slice/IDR).
 *
 * Parses SPS for resolution, profile, and level. Detects B-frames
 * via slice_type and computes display-order positions.
 */

#include "rfs_annexb.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <rss_common.h>
#include <raptor_hal.h>

/* ── Bitstream reader (exp-Golomb) ── */

typedef struct {
	const uint8_t *data;
	uint32_t size;
	uint32_t bit_pos;
	bool overflow;
} bits_t;

static void bits_init(bits_t *b, const uint8_t *data, uint32_t size)
{
	b->data = data;
	b->size = size;
	b->bit_pos = 0;
	b->overflow = false;
}

static uint32_t bits_read(bits_t *b, int n)
{
	uint32_t val = 0;
	for (int i = 0; i < n; i++) {
		uint32_t byte_pos = b->bit_pos / 8;
		if (byte_pos >= b->size) {
			b->overflow = true;
			return val << (n - i);
		}
		val = (val << 1) | ((b->data[byte_pos] >> (7 - (b->bit_pos & 7))) & 1);
		b->bit_pos++;
	}
	return val;
}

static void bits_skip(bits_t *b, int n)
{
	b->bit_pos += (uint32_t)n;
	if (b->bit_pos / 8 > b->size)
		b->overflow = true;
}

static uint32_t bits_ue(bits_t *b)
{
	int zeros = 0;
	while (!b->overflow && bits_read(b, 1) == 0 && zeros < 32)
		zeros++;
	return (1u << zeros) - 1 + bits_read(b, zeros);
}

static int32_t bits_se(bits_t *b)
{
	uint32_t v = bits_ue(b);
	return (v & 1) ? (int32_t)((v + 1) / 2) : -(int32_t)(v / 2);
}

/* ── RBSP de-emulation ── */

static uint32_t rbsp_unescape(const uint8_t *src, uint32_t len, uint8_t *dst, uint32_t cap)
{
	uint32_t out = 0;
	for (uint32_t i = 0; i < len && out < cap;) {
		if (i + 2 < len && src[i] == 0 && src[i + 1] == 0 && src[i + 2] == 3) {
			dst[out++] = 0;
			if (out < cap)
				dst[out++] = 0;
			i += 3;
		} else {
			dst[out++] = src[i++];
		}
	}
	return out;
}

/* ── NAL scanner ── */

static const uint8_t *find_sc(const uint8_t *p, const uint8_t *end, int *sc_len)
{
	while (p + 2 < end) {
		if (p[0] == 0 && p[1] == 0) {
			if (p[2] == 1) {
				*sc_len = 3;
				return p;
			}
			if (p + 3 < end && p[2] == 0 && p[3] == 1) {
				*sc_len = 4;
				return p;
			}
		}
		p++;
	}
	return NULL;
}

/* ── H.264 SPS parser ── */

static bool parse_h264_sps(const uint8_t *nal, uint32_t nal_len, rfs_annexb_info_t *info)
{
	if (nal_len < 5)
		return false;

	uint8_t rbsp[512];
	uint32_t rlen = rbsp_unescape(nal + 1, nal_len - 1, rbsp, sizeof(rbsp));

	bits_t b;
	bits_init(&b, rbsp, rlen);

	info->profile = (uint8_t)bits_read(&b, 8);
	bits_skip(&b, 8);
	info->level = (uint8_t)bits_read(&b, 8);
	bits_ue(&b);

	uint32_t chroma_format_idc = 1;
	if (info->profile >= 100) {
		chroma_format_idc = bits_ue(&b);
		if (chroma_format_idc == 3)
			bits_skip(&b, 1);
		bits_ue(&b);
		bits_ue(&b);
		bits_skip(&b, 1);
		if (bits_read(&b, 1)) {
			int cnt = (chroma_format_idc != 3) ? 8 : 12;
			for (int i = 0; i < cnt; i++) {
				if (bits_read(&b, 1)) {
					int size = (i < 6) ? 16 : 64;
					int last = 8, next = 8;
					for (int j = 0; j < size; j++) {
						if (next != 0)
							next = (last + bits_se(&b) + 256) % 256;
						last = next ? next : last;
					}
				}
			}
		}
	}

	bits_ue(&b);
	uint32_t poc_type = bits_ue(&b);
	if (poc_type == 0) {
		bits_ue(&b);
	} else if (poc_type == 1) {
		bits_skip(&b, 1);
		bits_se(&b);
		bits_se(&b);
		uint32_t n = bits_ue(&b);
		for (uint32_t i = 0; i < n; i++)
			bits_se(&b);
	}
	bits_ue(&b);
	bits_skip(&b, 1);

	uint32_t mbs_w = bits_ue(&b) + 1;
	uint32_t mbs_h = bits_ue(&b) + 1;
	uint32_t frame_mbs_only = bits_read(&b, 1);
	if (!frame_mbs_only)
		bits_skip(&b, 1);
	bits_skip(&b, 1);

	info->width = mbs_w * 16;
	info->height = mbs_h * 16 * (2 - frame_mbs_only);

	if (bits_read(&b, 1)) {
		uint32_t cux = (chroma_format_idc == 0) ? 1 : 2;
		uint32_t cuy = ((chroma_format_idc == 0) ? 1 : 2) * (2 - frame_mbs_only);
		uint32_t cl = bits_ue(&b), cr = bits_ue(&b);
		uint32_t ct = bits_ue(&b), cb = bits_ue(&b);
		uint32_t crop_w = (cl + cr) * cux;
		uint32_t crop_h = (ct + cb) * cuy;
		if (crop_w < info->width)
			info->width -= crop_w;
		if (crop_h < info->height)
			info->height -= crop_h;
	}

	if (b.overflow) {
		RSS_WARN("H.264 SPS parse: bitstream overflow");
		return false;
	}
	return true;
}

/* ── H.265 SPS parser ── */

static void skip_h265_ptl(bits_t *b, int max_sub_layers_m1, uint8_t *profile, uint8_t *level)
{
	bits_skip(b, 2);
	bits_skip(b, 1);
	*profile = (uint8_t)bits_read(b, 5);
	bits_skip(b, 32);
	bits_skip(b, 48);
	*level = (uint8_t)bits_read(b, 8);

	if (max_sub_layers_m1 == 0)
		return;

	bool sub_prof[8] = {0}, sub_lev[8] = {0};
	for (int i = 0; i < max_sub_layers_m1; i++) {
		sub_prof[i] = bits_read(b, 1);
		sub_lev[i] = bits_read(b, 1);
	}
	for (int i = max_sub_layers_m1; i < 8; i++)
		bits_skip(b, 2);
	for (int i = 0; i < max_sub_layers_m1; i++) {
		if (sub_prof[i])
			bits_skip(b, 88);
		if (sub_lev[i])
			bits_skip(b, 8);
	}
}

static bool parse_h265_sps(const uint8_t *nal, uint32_t nal_len, rfs_annexb_info_t *info)
{
	if (nal_len < 6)
		return false;

	uint8_t rbsp[512];
	uint32_t rlen = rbsp_unescape(nal + 2, nal_len - 2, rbsp, sizeof(rbsp));

	bits_t b;
	bits_init(&b, rbsp, rlen);

	bits_skip(&b, 4);
	int max_sub_layers_m1 = (int)bits_read(&b, 3);
	bits_skip(&b, 1);

	skip_h265_ptl(&b, max_sub_layers_m1, &info->profile, &info->level);

	bits_ue(&b);
	uint32_t chroma_format_idc = bits_ue(&b);
	if (chroma_format_idc == 3)
		bits_skip(&b, 1);

	info->width = bits_ue(&b);
	info->height = bits_ue(&b);

	if (bits_read(&b, 1)) {
		uint32_t sub_w = (chroma_format_idc == 1 || chroma_format_idc == 2) ? 2 : 1;
		uint32_t sub_h = (chroma_format_idc == 1) ? 2 : 1;
		uint32_t cl = bits_ue(&b), cr = bits_ue(&b);
		uint32_t ct = bits_ue(&b), cb = bits_ue(&b);
		uint32_t crop_w = (cl + cr) * sub_w;
		uint32_t crop_h = (ct + cb) * sub_h;
		if (crop_w < info->width)
			info->width -= crop_w;
		if (crop_h < info->height)
			info->height -= crop_h;
	}

	if (b.overflow) {
		RSS_WARN("H.265 SPS parse: bitstream overflow");
		return false;
	}
	return true;
}

/* ── Slice type (B-frame detection) ── */

static int h264_slice_type(const uint8_t *nal, uint32_t nal_len)
{
	if (nal_len < 3)
		return -1;
	bits_t b;
	bits_init(&b, nal + 1, nal_len - 1);
	bits_ue(&b);
	uint32_t st = bits_ue(&b);
	if (b.overflow)
		return -1;
	return (st % 5 == 1) ? 1 : 0;
}

static int h265_slice_type(const uint8_t *nal, uint32_t nal_len)
{
	if (nal_len < 4)
		return -1;
	uint8_t nt = (nal[0] >> 1) & 0x3F;
	if (nt >= 16 && nt <= 23)
		return 0;
	bits_t b;
	bits_init(&b, nal + 2, nal_len - 2);
	uint32_t first_slice = bits_read(&b, 1);
	if (!first_slice)
		return -1;
	bits_ue(&b);
	uint32_t st = bits_ue(&b);
	if (b.overflow)
		return -1;
	return (st == 0) ? 1 : 0;
}

/* ── Display order computation ── */

static void compute_display_order(rfs_frame_t *frames, uint32_t count)
{
	bool has_bframes = false;
	for (uint32_t i = 0; i < count; i++) {
		if (frames[i].is_bframe) {
			has_bframes = true;
			break;
		}
	}

	if (!has_bframes) {
		for (uint32_t i = 0; i < count; i++)
			frames[i].display_pos = i;
		return;
	}

	uint32_t display = 0;
	int pending_ref = -1;

	for (uint32_t i = 0; i < count; i++) {
		if (frames[i].is_bframe) {
			frames[i].display_pos = display++;
		} else {
			if (pending_ref >= 0)
				frames[pending_ref].display_pos = display++;
			pending_ref = (int)i;
		}
	}
	if (pending_ref >= 0)
		frames[pending_ref].display_pos = display;

	RSS_INFO("B-frame reorder applied (%u frames)", count);
}

/* ── Codec detection ── */

static int detect_codec(uint8_t first_byte)
{
	if (first_byte == 0x67 || first_byte == 0x47 || first_byte == 0x27)
		return RSS_CODEC_H264;
	if (first_byte == 0x68)
		return RSS_CODEC_H264;
	if (first_byte == 0x65)
		return RSS_CODEC_H264;
	if (first_byte == 0x09)
		return RSS_CODEC_H264;
	if (first_byte == 0x40 || first_byte == 0x42 || first_byte == 0x44 || first_byte == 0x46)
		return RSS_CODEC_H265;
	if (first_byte == 0x26)
		return RSS_CODEC_H265;
	return -1;
}

static int detect_codec_from_extension(const char *path)
{
	const char *dot = strrchr(path, '.');
	if (!dot)
		return -1;
	if (strcasecmp(dot, ".h264") == 0 || strcasecmp(dot, ".264") == 0)
		return RSS_CODEC_H264;
	if (strcasecmp(dot, ".h265") == 0 || strcasecmp(dot, ".265") == 0 ||
	    strcasecmp(dot, ".hevc") == 0)
		return RSS_CODEC_H265;
	return -1;
}

/* ── NAL type classification ── */

static bool h264_is_vcl(uint8_t b)
{
	uint8_t t = b & 0x1F;
	return t >= 1 && t <= 5;
}
static bool h264_is_idr(uint8_t b)
{
	return (b & 0x1F) == 5;
}
static bool h264_is_sps(uint8_t b)
{
	return (b & 0x1F) == 7;
}

static uint16_t h264_map_nal(uint8_t b)
{
	switch (b & 0x1F) {
	case 5:
		return RSS_NAL_H264_IDR;
	case 7:
		return RSS_NAL_H264_SPS;
	case 8:
		return RSS_NAL_H264_PPS;
	case 6:
		return RSS_NAL_H264_SEI;
	default:
		return RSS_NAL_H264_SLICE;
	}
}

static bool h265_is_vcl(uint8_t b)
{
	return ((b >> 1) & 0x3F) <= 31;
}
static bool h265_is_idr(uint8_t b)
{
	uint8_t t = (b >> 1) & 0x3F;
	return t >= 16 && t <= 21;
}
static bool h265_is_sps(uint8_t b)
{
	return ((b >> 1) & 0x3F) == 33;
}

static uint16_t h265_map_nal(uint8_t b)
{
	uint8_t t = (b >> 1) & 0x3F;
	if (t >= 16 && t <= 21)
		return RSS_NAL_H265_IDR;
	if (t == 32)
		return RSS_NAL_H265_VPS;
	if (t == 33)
		return RSS_NAL_H265_SPS;
	if (t == 34)
		return RSS_NAL_H265_PPS;
	if (t == 39 || t == 40)
		return RSS_NAL_H265_SEI;
	return RSS_NAL_H265_SLICE;
}

/* ── Public API ── */

int rfs_annexb_scan(const uint8_t *data, size_t size, const char *path, int codec_hint,
		    rfs_annexb_info_t *info)
{
	memset(info, 0, sizeof(*info));

	const uint8_t *end = data + size;
	bool sps_parsed = false;
	int codec;

	/* Codec detection */
	if (codec_hint >= 0) {
		codec = codec_hint;
	} else {
		codec = detect_codec_from_extension(path);
		if (codec < 0) {
			int sc_len;
			const uint8_t *sc = find_sc(data, end, &sc_len);
			while (sc && codec < 0) {
				const uint8_t *nal = sc + sc_len;
				if (nal < end)
					codec = detect_codec(nal[0]);
				int next_len;
				sc = find_sc(nal, end, &next_len);
				sc_len = next_len;
			}
		}
	}

	if (codec < 0) {
		RSS_ERROR("cannot detect video codec -- set codec in config");
		return -1;
	}

	info->codec = codec;
	RSS_INFO("codec: %s", codec == RSS_CODEC_H265 ? "H.265" : "H.264");

	bool (*is_vcl)(uint8_t) = (codec == RSS_CODEC_H265) ? h265_is_vcl : h264_is_vcl;
	bool (*is_idr)(uint8_t) = (codec == RSS_CODEC_H265) ? h265_is_idr : h264_is_idr;
	bool (*is_sps)(uint8_t) = (codec == RSS_CODEC_H265) ? h265_is_sps : h264_is_sps;
	uint16_t (*map_nal)(uint8_t) = (codec == RSS_CODEC_H265) ? h265_map_nal : h264_map_nal;

	info->frames = calloc(RFS_ANNEXB_MAX_FRAMES, sizeof(rfs_frame_t));
	if (!info->frames) {
		RSS_ERROR("frame index alloc failed");
		return -1;
	}

	int sc_len;
	const uint8_t *sc = find_sc(data, end, &sc_len);
	uint32_t frame_start_off = sc ? (uint32_t)(sc - data) : 0;

	while (sc) {
		const uint8_t *nal = sc + sc_len;
		if (nal >= end)
			break;

		int next_sc_len = 0;
		const uint8_t *next_sc = find_sc(nal, end, &next_sc_len);
		uint32_t nal_len = next_sc ? (uint32_t)(next_sc - nal) : (uint32_t)(end - nal);

		while (nal_len > 1 && nal[nal_len - 1] == 0)
			nal_len--;

		if (!sps_parsed && is_sps(nal[0])) {
			bool ok = (codec == RSS_CODEC_H265) ? parse_h265_sps(nal, nal_len, info)
							    : parse_h264_sps(nal, nal_len, info);
			if (ok) {
				sps_parsed = true;
				RSS_INFO("SPS: %ux%u profile=%u level=%u", info->width,
					 info->height, info->profile, info->level);
			} else {
				RSS_WARN("SPS parse failed, will retry on next SPS");
			}
		}

		if (is_vcl(nal[0])) {
			uint32_t frame_end = next_sc ? (uint32_t)(next_sc - data) : (uint32_t)size;

			if (info->frame_count >= RFS_ANNEXB_MAX_FRAMES) {
				RSS_WARN("frame limit reached (%u)", RFS_ANNEXB_MAX_FRAMES);
				break;
			}

			int stype = (codec == RSS_CODEC_H265) ? h265_slice_type(nal, nal_len)
							      : h264_slice_type(nal, nal_len);

			rfs_frame_t *f = &info->frames[info->frame_count];
			f->offset = frame_start_off;
			f->length = frame_end - frame_start_off;
			f->nal_type = map_nal(nal[0]);
			f->is_key = is_idr(nal[0]) ? 1 : 0;
			f->is_bframe = (stype == 1) ? 1 : 0;
			info->frame_count++;

			frame_start_off = frame_end;
		}

		sc = next_sc;
		sc_len = next_sc_len;
	}

	if (info->frame_count == 0) {
		RSS_ERROR("no video frames found");
		free(info->frames);
		info->frames = NULL;
		return -1;
	}

	compute_display_order(info->frames, info->frame_count);

	uint32_t keys = 0;
	for (uint32_t i = 0; i < info->frame_count; i++)
		if (info->frames[i].is_key)
			keys++;

	RSS_INFO("indexed %u frames (%u keyframes)", info->frame_count, keys);
	return 0;
}

void rfs_annexb_free(rfs_annexb_info_t *info)
{
	free(info->frames);
	memset(info, 0, sizeof(*info));
}

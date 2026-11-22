/*
 * rcd_ethcam.c -- Novatek EthCam wire protocol (see rcd_ethcam.h)
 */

#include "rcd_ethcam.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int ethcam_build_cmd(char *dst, size_t cap, const char *ip, int port, int cmd, int par)
{
	int n = snprintf(dst, cap, "%s:%d/?custom=1&cmd=%d&par=%d", ip, port, cmd, par);
	if (n < 0 || (size_t)n + 1 > cap)
		return -1;
	return n + 1;
}

static const char *find_in(const char *s, const char *end, const char *pat)
{
	size_t pl = strlen(pat);
	for (; s + pl <= end; s++) {
		if (memcmp(s, pat, pl) == 0)
			return s;
	}
	return NULL;
}

/* Value of <tag>...</tag> inside [s, end) as text, or NULL */
static const char *tag_text(const char *s, const char *end, const char *tag, char *out, size_t cap)
{
	char open[24], close[24];
	snprintf(open, sizeof(open), "<%s>", tag);
	snprintf(close, sizeof(close), "</%s>", tag);
	const char *a = find_in(s, end, open);
	if (!a)
		return NULL;
	a += strlen(open);
	const char *b = find_in(a, end, close);
	if (!b)
		return NULL;
	size_t n = (size_t)(b - a);
	if (n >= cap)
		n = cap - 1;
	memcpy(out, a, n);
	out[n] = '\0';
	return b + strlen(close);
}

int ethcam_parse_reply(const char *buf, size_t len, ethcam_reply_t *out)
{
	const char *end = buf + len;
	const char *fe = find_in(buf, end, "</Function>");
	const char *le = find_in(buf, end, "</LIST>");
	const char *close;
	size_t close_len;

	if (fe && (!le || fe < le)) {
		close = fe;
		close_len = strlen("</Function>");
	} else if (le) {
		close = le;
		close_len = strlen("</LIST>");
	} else {
		return 0;
	}
	const char *stop = close + close_len;
	if (stop < end && *stop == '\n')
		stop++;

	memset(out, 0, sizeof(*out));
	out->status = INT_MIN;
	char tmp[80];
	if (tag_text(buf, close, "Cmd", tmp, sizeof(tmp)))
		out->cmd = atoi(tmp);
	if (tag_text(buf, close, "Status", tmp, sizeof(tmp)))
		out->status = atoi(tmp);
	if (tag_text(buf, close, "Value", tmp, sizeof(tmp)))
		out->value = atoll(tmp);
	tag_text(buf, close, "String", out->string, sizeof(out->string));

	const char *p = buf;
	while (out->item_count < ETHCAM_MAX_ITEMS &&
	       (p = tag_text(p, close, "ITEM", tmp, sizeof(tmp))) != NULL)
		out->items[out->item_count++] = atoi(tmp);

	return (int)(stop - buf);
}

static bool is_start_code(const uint8_t *p)
{
	return p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1;
}

int ethcam_parse_frame(const uint8_t *buf, size_t len, bool h265, ethcam_frame_t *out)
{
	size_t h = 0;

	memset(out, 0, sizeof(*out));
	if (len < 16)
		return 0;

	if (!h265 && len >= 7 && is_start_code(buf + 2) && (buf[6] & 0x1f) == 7) {
		uint32_t sl = (uint32_t)buf[0] | (uint32_t)buf[1] << 8;
		if (sl < 5 || sl > 512)
			return -1;
		if (len < 4 + sl)
			return 0;
		uint32_t pl = (uint32_t)buf[2 + sl] | (uint32_t)buf[3 + sl] << 8;
		if (pl < 5 || pl > 512)
			return -1;
		if (len < 4 + sl + pl + 16)
			return 0;
		out->sps = buf + 2;
		out->sps_len = sl;
		out->pps = buf + 4 + sl;
		out->pps_len = pl;
		if (!is_start_code(out->sps) || !is_start_code(out->pps))
			return -1;
		h = 4 + sl + pl;
	}

	const uint8_t *hd = buf + h;
	if (hd[15] != 2)
		return -1;
	uint32_t size = (uint32_t)hd[8] | (uint32_t)hd[9] << 8 | (uint32_t)(hd[10] & 15) << 16;
	if (size < 5)
		return -1;
	if (len < h + 16 + size)
		return 0;

	const uint8_t *body = hd + 16;
	if (!is_start_code(body))
		return -1;

	uint64_t ts = 0;
	for (int i = 0; i < 8; i++)
		ts = ts << 8 | hd[i];

	uint8_t ca = 0, cb = 0;
	if (size > 64) {
		for (uint32_t i = 0; i < 64; i++) {
			ca = (uint8_t)(ca + body[i]);
			cb = (uint8_t)(cb + body[size - 64 + i]);
		}
	}

	out->body = body;
	out->body_len = size;
	out->ts_us = ts;
	out->gop_idx = hd[11];
	out->chk_ok = ca == hd[12] && cb == hd[13];
	out->nal = h265 ? (body[4] >> 1) & 0x3f : body[4] & 0x1f;
	out->key = out->gop_idx == 1;
	return (int)(h + 16 + size);
}

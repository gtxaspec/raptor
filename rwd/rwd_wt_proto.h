/*
 * rwd_wt_proto.h -- WebTorrent tracker wire forms
 *
 * Trackers take info_hash, peer_id, to_peer_id and offer_id as 20-byte
 * binary strings: one JSON character per byte, U+0000..U+00FF (cJSON
 * carries them as UTF-8). They relay only "type" and "sdp" of an offer or
 * answer, so anything else raptor sends rides in the SDP as an attribute.
 */

#ifndef RWD_WT_PROTO_H
#define RWD_WT_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define WT_ID_LEN 20

/* Bytes to a binary string in UTF-8. out needs 2 * n + 1 bytes. */
static inline void wt_bin_to_json(const uint8_t *in, size_t n, char *out)
{
	char *o = out;
	for (size_t i = 0; i < n; i++) {
		if (in[i] < 0x80) {
			*o++ = (char)in[i];
		} else {
			*o++ = (char)(0xc0 | (in[i] >> 6));
			*o++ = (char)(0x80 | (in[i] & 0x3f));
		}
	}
	*o = '\0';
}

/* A binary string (UTF-8) back to exactly n bytes; false for any other
 * length or a character above U+00FF. */
static inline bool wt_json_to_bin(const char *s, uint8_t *out, size_t n)
{
	const uint8_t *p = (const uint8_t *)s;
	size_t i = 0;
	while (*p) {
		if (i == n)
			return false;
		if (*p < 0x80) {
			out[i++] = *p++;
		} else if ((p[0] & 0xfe) == 0xc2 && (p[1] & 0xc0) == 0x80) {
			out[i++] = (uint8_t)((p[0] & 0x03) << 6 | (p[1] & 0x3f));
			p += 2;
		} else {
			return false;
		}
	}
	return i == n;
}

/*
 * Find the attribute line "a=<name>:<value>" in an SDP, copy its value and
 * remove the line, CRLF or LF included. Returns false (SDP untouched) when
 * the line is missing or its value does not fit.
 */
static inline bool wt_sdp_take_attr(char *sdp, const char *name, char *val, size_t val_size)
{
	size_t nlen = strlen(name);
	char *line = sdp;
	while (line && *line) {
		if (line[0] == 'a' && line[1] == '=' && strncmp(line + 2, name, nlen) == 0 &&
		    line[2 + nlen] == ':') {
			const char *v = line + 3 + nlen;
			size_t vlen = strcspn(v, "\r\n");
			if (vlen >= val_size)
				return false;
			memcpy(val, v, vlen);
			val[vlen] = '\0';
			const char *end = v + vlen;
			if (*end == '\r')
				end++;
			if (*end == '\n')
				end++;
			memmove(line, end, strlen(end) + 1);
			return true;
		}
		line = strchr(line, '\n');
		if (line)
			line++;
	}
	return false;
}

#endif

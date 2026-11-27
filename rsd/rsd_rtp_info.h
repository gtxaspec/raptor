/*
 * rsd_rtp_info.h -- the RTP-Info value of a PLAY reply (RFC 2326 12.33)
 */

#ifndef RSD_RTP_INFO_H
#define RSD_RTP_INFO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
	bool on;
	uint16_t seq;
	uint32_t rtptime;
} rsd_rtp_info_track_t;

/* Where the URI continues after any "user:pass@" of its authority: RFC 3986
 * 3.2 ends the authority at the first '/', '?' or '#', and userinfo at the
 * last '@' inside it (a password may carry a raw '@'). */
static inline size_t rsd_uri_after_userinfo(const char *uri, size_t len)
{
	size_t i = 0;
	while (i + 2 < len && uri[i] != '/' && uri[i] != '?' && uri[i] != '#' &&
	       !(uri[i] == ':' && uri[i + 1] == '/' && uri[i + 2] == '/'))
		i++;
	if (i + 2 >= len || uri[i] != ':')
		return 0;
	size_t start = i + 3, at = 0;
	for (size_t j = start; j < len && uri[j] != '/' && uri[j] != '?' && uri[j] != '#'; j++) {
		if (uri[j] == '@')
			at = j + 1;
	}
	return at;
}

/* "url=<uri>/video;seq=..;rtptime=..[,url=<uri>/audio;...]" for the request
 * URI without its credentials (ffmpeg strips them from the URL it asked for
 * and fails to match an RTP-Info url that still has them), trailing '/'
 * dropped. Returns the length, 0 with neither track on, or -1 when it does
 * not fit in cap: the URI is the client's and as long as it likes. */
static inline int rsd_rtp_info_build(char *out, size_t cap, const char *uri, size_t uri_len,
				     const rsd_rtp_info_track_t *video,
				     const rsd_rtp_info_track_t *audio)
{
	const rsd_rtp_info_track_t *track[2] = {video, audio};
	static const char *const name[2] = {"video", "audio"};
	size_t off = 0;

	if (cap == 0)
		return -1;
	out[0] = '\0';
	while (uri_len > 0 && uri[uri_len - 1] == '/')
		uri_len--;
	size_t scheme_len = 0, after = rsd_uri_after_userinfo(uri, uri_len);
	if (after) {
		const char *sep = memchr(uri, ':', uri_len);
		scheme_len = (size_t)(sep - uri) + 3;
	}
	for (int i = 0; i < 2; i++) {
		if (!track[i]->on)
			continue;
		/* snprintf returns what it wanted to write: adding that to off
		 * unchecked walks off past the buffer */
		int n = snprintf(out + off, cap - off, "%surl=%.*s%.*s/%s;seq=%u;rtptime=%u",
				 off ? "," : "", (int)scheme_len, uri, (int)(uri_len - after),
				 uri + after, name[i], track[i]->seq, track[i]->rtptime);
		if (n < 0 || (size_t)n >= cap - off) {
			out[0] = '\0';
			return -1;
		}
		off += (size_t)n;
	}
	return (int)off;
}

#endif

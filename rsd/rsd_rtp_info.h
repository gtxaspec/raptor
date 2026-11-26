/*
 * rsd_rtp_info.h -- the RTP-Info value of a PLAY reply (RFC 2326 12.33)
 */

#ifndef RSD_RTP_INFO_H
#define RSD_RTP_INFO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
	bool on;
	uint16_t seq;
	uint32_t rtptime;
} rsd_rtp_info_track_t;

/* "url=<uri>/video;seq=..;rtptime=..[,url=<uri>/audio;...]" for the request
 * URI, trailing '/' dropped. Returns the length, 0 with neither track on,
 * or -1 when it does not fit in cap: the URI is the client's and as long as
 * it likes. */
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
	for (int i = 0; i < 2; i++) {
		if (!track[i]->on)
			continue;
		/* snprintf returns what it wanted to write: adding that to off
		 * unchecked walks off past the buffer */
		int n = snprintf(out + off, cap - off, "%surl=%.*s/%s;seq=%u;rtptime=%u",
				 off ? "," : "", (int)uri_len, uri, name[i], track[i]->seq,
				 track[i]->rtptime);
		if (n < 0 || (size_t)n >= cap - off) {
			out[0] = '\0';
			return -1;
		}
		off += (size_t)n;
	}
	return (int)off;
}

#endif

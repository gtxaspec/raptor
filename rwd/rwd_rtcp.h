/*
 * rwd_rtcp.h -- header-only RTCP feedback classification.
 */
#ifndef RWD_RTCP_H
#define RWD_RTCP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline bool rwd_rtcp_requests_keyframe(const uint8_t *packet, size_t length)
{
	uint8_t fmt;

	if (!packet || length < 12 || (packet[0] & 0xc0U) != 0x80U)
		return false;
	fmt = packet[0] & 0x1fU;
	return packet[1] == 206U && (fmt == 1U || fmt == 4U);
}

#endif

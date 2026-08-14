/*
 * rsp_net.c -- serverless RTP push over UDP
 *
 * The packetizer is compy's NAL transport, the exact stack rsd's UDP
 * mode ships through, so fragmentation and both codecs come proven.
 * What this file adds is only the socket and the posture: connect a
 * non-blocking UDP socket to the configured destination and send each
 * frame the moment the ring delivers it. There is no reconnect state
 * because there is no connection, and nothing is ever queued -- if
 * the socket pushes back the frame is dropped and counted. SPS/PPS
 * ride in-band on every IDR, so a receiver can lock mid-stream; the
 * first send is still held for a keyframe so a fresh receiver never
 * starts inside a GOP.
 */

#include "rsp_net.h"

#include <compy.h>

#include <rss_common.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define RSP_NET_WARN_INTERVAL_US 5000000

int rsp_net_parse_url(const char *url, char *host, size_t host_size, int *port)
{
	const char *p = url;

	if (strncmp(p, "udp://", 6) != 0)
		return -1;
	p += 6;

	const char *host_start, *host_end;
	if (*p == '[') {
		/* Bracketed IPv6 literal */
		host_start = p + 1;
		host_end = strchr(host_start, ']');
		if (!host_end)
			return -1;
		p = host_end + 1;
	} else {
		host_start = p;
		host_end = strchr(host_start, ':');
		if (!host_end)
			return -1;
		p = host_end;
	}

	if (*p != ':')
		return -1;
	long val = strtol(p + 1, NULL, 10);
	if (val < 1 || val > 65535)
		return -1;

	size_t hlen = (size_t)(host_end - host_start);
	if (hlen == 0 || hlen >= host_size)
		return -1;
	memcpy(host, host_start, hlen);
	host[hlen] = '\0';
	*port = (int)val;
	return 0;
}

int rsp_net_open(rsp_net_t *n, const char *host, int port)
{
	char portstr[16];
	snprintf(portstr, sizeof(portstr), "%d", port);

	struct addrinfo hints = {0};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;

	struct addrinfo *res = NULL;
	int ret = getaddrinfo(host, portstr, &hints, &res);
	if (ret != 0) {
		RSS_WARN("resolve %s failed: %s", host, gai_strerror(ret));
		return -1;
	}

	/* IPv6 first, IPv4 as the fallback. */
	struct addrinfo *pick = NULL;
	for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
		if (ai->ai_family == AF_INET6) {
			pick = ai;
			break;
		}
	}
	if (!pick) {
		for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
			if (ai->ai_family == AF_INET) {
				pick = ai;
				break;
			}
		}
	}
	if (!pick) {
		freeaddrinfo(res);
		RSS_WARN("resolve %s: no usable address", host);
		return -1;
	}

	int fd = socket(pick->ai_family, SOCK_DGRAM, 0);
	if (fd < 0) {
		freeaddrinfo(res);
		RSS_WARN("socket: %s", strerror(errno));
		return -1;
	}
	if (connect(fd, pick->ai_addr, pick->ai_addrlen) < 0) {
		RSS_WARN("connect %s:%d: %s", host, port, strerror(errno));
		close(fd);
		freeaddrinfo(res);
		return -1;
	}
	freeaddrinfo(res);
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

	Compy_Transport t = compy_transport_udp(fd);
	n->rtp = Compy_RtpTransport_new(t, 96, 90000);
	n->nal = Compy_NalTransport_new(n->rtp);
	n->fd = fd;
	n->wait_key = true;
	n->send_errors = 0;
	n->last_err_us = 0;
	return 0;
}

int rsp_net_send_video(rsp_net_t *n, const uint8_t *data, uint32_t len, int64_t timestamp_us,
		       bool is_key, uint32_t codec)
{
	if (n->fd < 0)
		return 0;
	if (n->wait_key) {
		if (!is_key)
			return 0;
		n->wait_key = false;
	}

	/* 90 kHz wire clock from the ring's microsecond capture stamp. */
	uint32_t rtp_ts = (uint32_t)((uint64_t)timestamp_us * 9 / 100);

	bool is_h265 = (codec == 1);
	int hdr_size = is_h265 ? 2 : 1;
	int sent = 0;

	const uint8_t *p = data;
	const uint8_t *end = data + len;

	while (p + 4 < end) {
		if (!(p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1)) {
			p++;
			continue;
		}

		const uint8_t *nalu_start = p + 4;
		const uint8_t *nalu_end = end;
		for (const uint8_t *q = nalu_start + 1; q + 3 < end; q++) {
			if (q[0] == 0 && q[1] == 0 && q[2] == 0 && q[3] == 1) {
				nalu_end = q;
				break;
			}
		}

		uint32_t nalu_len = (uint32_t)(nalu_end - nalu_start);
		if (nalu_len < (uint32_t)hdr_size) {
			p = nalu_end;
			continue;
		}

		Compy_NalUnit nalu;
		if (is_h265) {
			nalu = (Compy_NalUnit){
				.header = Compy_NalHeader_H265(
					Compy_H265NalHeader_parse((uint8_t *)nalu_start)),
				.payload = U8Slice99_new((uint8_t *)(nalu_start + 2), nalu_len - 2),
			};
		} else {
			nalu = (Compy_NalUnit){
				.header = Compy_NalHeader_H264(
					Compy_H264NalHeader_parse(nalu_start[0])),
				.payload = U8Slice99_new((uint8_t *)(nalu_start + 1), nalu_len - 1),
			};
		}

		if (Compy_NalTransport_send_packet(n->nal, Compy_RtpTimestamp_Raw(rtp_ts), nalu) <
		    0) {
			n->send_errors++;
			int64_t now = rss_timestamp_us();
			if (now - n->last_err_us > RSP_NET_WARN_INTERVAL_US) {
				n->last_err_us = now;
				RSS_WARN("udp send failing (%s), %" PRIu64 " errors",
					 strerror(errno), n->send_errors);
			}
		} else {
			sent++;
		}

		p = nalu_end;
	}

	return sent;
}

void rsp_net_close(rsp_net_t *n)
{
	if (n->nal) {
		/* Dropping the NAL transport cascades to the RTP transport
		 * and the UDP wrapper; the fd stays ours to close. */
		VCALL(DYN(Compy_NalTransport, Compy_Droppable, n->nal), drop);
		n->nal = NULL;
		n->rtp = NULL;
	}
	if (n->fd >= 0) {
		close(n->fd);
		n->fd = -1;
	}
}

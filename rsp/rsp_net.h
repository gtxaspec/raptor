/*
 * rsp_net.h -- serverless RTP push over UDP
 *
 * Packetizes ring video with the same RTP/NAL stack rsd's UDP
 * transport uses and sends it to a fixed host:port. No session, no
 * handshake, no retransmission: the consumer is whatever is bound on
 * the far port. Video only.
 */

#ifndef RSP_NET_H
#define RSP_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct Compy_RtpTransport;
struct Compy_NalTransport;

typedef struct {
	int fd; /* -1 when closed; owned here, not by compy */
	struct Compy_RtpTransport *rtp;
	struct Compy_NalTransport *nal;
	bool wait_key; /* hold sends until a keyframe starts the stream */
	uint64_t send_errors;
	int64_t last_err_us; /* rate limit for send-failure warnings */
} rsp_net_t;

/* Parse udp://host:port (IPv6 literals in brackets). 0 on success. */
int rsp_net_parse_url(const char *url, char *host, size_t host_size, int *port);

/* Resolve, connect and build the RTP stack. 0 on success. */
int rsp_net_open(rsp_net_t *n, const char *host, int port);

/*
 * Send one Annex B frame as RTP packets. Never blocks: a send the
 * socket refuses is dropped and counted, because a stale frame is
 * worth less than a missing one. Returns the number of NAL units
 * sent (0 while waiting for the first keyframe).
 */
int rsp_net_send_video(rsp_net_t *n, const uint8_t *data, uint32_t len, int64_t timestamp_us,
		       bool is_key, uint32_t codec);

void rsp_net_close(rsp_net_t *n);

#endif /* RSP_NET_H */

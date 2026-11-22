/*
 * rcd_ethcam.h -- Novatek EthCam (rear camera module) wire protocol
 *
 * The module is a TCP server: commands on 8899 (ASCII request, XML reply),
 * record stream on 8887, display stream on 8888. Each frame on a data
 * socket is a 16-byte header plus Annex-B bitstream; H.264 I frames are
 * preceded by length-prefixed SPS/PPS. Pure functions, no I/O.
 */

#ifndef RCD_ETHCAM_H
#define RCD_ETHCAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ETHCAM_PORT_CMD		 8899
#define ETHCAM_PORT_REC		 8887
#define ETHCAM_PORT_DISP	 8888

#define ETHCAM_CMD_STARTUP	 9001
#define ETHCAM_CMD_GET_SPS_PPS	 9007
#define ETHCAM_CMD_STREAM_START	 9008
#define ETHCAM_CMD_STREAM_STOP	 9009
#define ETHCAM_CMD_GET_REC_INFO	 9012
#define ETHCAM_CMD_SYNC_TIME	 9014
#define ETHCAM_CMD_RESET_I	 9016
#define ETHCAM_CMD_GET_DISP_INFO 9017
#define ETHCAM_CMD_FW_SEND	 9020
#define ETHCAM_CMD_FW_START	 9021
#define ETHCAM_CMD_FW_VERSION	 9025
#define ETHCAM_CMD_SET_SYS_INFO	 9027
#define ETHCAM_CMD_SET_CODEC	 9032

/* Reply status values */
#define ETHCAM_ST_ACK	   0
#define ETHCAM_ST_OK	   1
#define ETHCAM_ST_CONTINUE 2

/* 20-bit size field: no frame can be larger */
#define ETHCAM_MAX_FRAME (1u << 20)

#define ETHCAM_MAX_ITEMS 40

typedef struct {
	int cmd;
	int status;	 /* INT32_MIN when the reply has none */
	long long value; /* <Value> */
	char string[64]; /* <String> */
	int items[ETHCAM_MAX_ITEMS];
	int item_count; /* <LIST> replies */
} ethcam_reply_t;

/* "<ip>:<port>/?custom=1&cmd=N&par=P" plus the terminating NUL, which the
 * module requires. Returns the byte count to send, or -1. */
int ethcam_build_cmd(char *dst, size_t cap, const char *ip, int port, int cmd, int par);

/* Find one complete reply in buf (replies are `ip:port` + XML, no length
 * field). Returns bytes consumed, 0 if more data is needed. */
int ethcam_parse_reply(const char *buf, size_t len, ethcam_reply_t *out);

typedef struct {
	/* H.264 I frames carry SPS/PPS before the header; Annex-B, start codes included */
	const uint8_t *sps, *pps;
	uint32_t sps_len, pps_len;
	const uint8_t *body; /* Annex-B, starts with 00 00 00 01 */
	uint32_t body_len;
	uint64_t ts_us; /* module clock */
	int gop_idx;	/* 1 = I frame */
	bool key;
	bool chk_ok;
	int nal; /* first VCL/param NAL type in body (codec-specific) */
} ethcam_frame_t;

/* Parse one record from the start of buf. Returns bytes consumed, 0 if more
 * data is needed, -1 if the stream is out of sync. */
int ethcam_parse_frame(const uint8_t *buf, size_t len, bool h265, ethcam_frame_t *out);

#endif

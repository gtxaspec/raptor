/*
 * rcd_drv_ethcam.c -- driver for Novatek EthCam rear camera modules
 *
 * Bring-up as the stock receiver does it (raptor-docs, external cameras):
 * 9027 system info, 9032 codec, 9001, 9014 time, 9012/9017 stream info,
 * connect the data sockets, 9008 display then record. Two-phase commands
 * send their binary payload in one write: the module reads it with one
 * receive. Data sockets are never half-closed: the module drops such a
 * client and sends nothing.
 */

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <raptor_hal.h>

#include "rcd.h"
#include "rcd_ethcam.h"

#define CMD_TIMEOUT_MS 3000
#define HEARTBEAT_US   5000000
#define CMD_DEAD_US    15000000
#define RX_BUF	       (2 * ETHCAM_MAX_FRAME)

typedef struct {
	int cmd_fd;
	char cmd_buf[8192];
	size_t cmd_len;
	int64_t cmd_last_rx_us;
	int enc_buf_sec;
	int fd[2];
	uint8_t *buf[2];
	size_t len[2];
} ethcam_t;

static const int data_port[2] = {ETHCAM_PORT_REC, ETHCAM_PORT_DISP};

static int cmd_send(rcd_cam_t *c, int port, int cmd, int par)
{
	ethcam_t *e = c->priv;
	char req[96];
	int n = ethcam_build_cmd(req, sizeof(req), c->ip, port, cmd, par);
	if (n < 0)
		return -1;
	return rcd_send_all(e->cmd_fd, req, (size_t)n, CMD_TIMEOUT_MS);
}

static int cmd_recv(ethcam_t *e)
{
	if (e->cmd_len == sizeof(e->cmd_buf))
		e->cmd_len = 0; /* no reply end in a full buffer: garbage */
	ssize_t n = recv(e->cmd_fd, e->cmd_buf + e->cmd_len, sizeof(e->cmd_buf) - e->cmd_len, 0);
	if (n <= 0)
		return -1;
	e->cmd_len += (size_t)n;
	e->cmd_last_rx_us = rss_timestamp_us();
	return 0;
}

static int cmd_next(ethcam_t *e, ethcam_reply_t *r)
{
	int used = ethcam_parse_reply(e->cmd_buf, e->cmd_len, r);
	if (used > 0) {
		memmove(e->cmd_buf, e->cmd_buf + used, e->cmd_len - (size_t)used);
		e->cmd_len -= (size_t)used;
	}
	return used;
}

/* Wait for the reply to want_cmd, skipping others (heartbeats, keyframe acks) */
static int cmd_wait(ethcam_t *e, int want_cmd, ethcam_reply_t *r)
{
	int64_t deadline = rss_timestamp_us() + CMD_TIMEOUT_MS * 1000;
	for (;;) {
		while (cmd_next(e, r) > 0) {
			if (r->cmd == want_cmd)
				return 0;
		}
		int64_t left = deadline - rss_timestamp_us();
		if (left <= 0)
			return -1;
		struct pollfd p = {.fd = e->cmd_fd, .events = POLLIN};
		if (poll(&p, 1, (int)(left / 1000) + 1) != 1 || cmd_recv(e) < 0)
			return -1;
	}
}

static int cmd_do(rcd_cam_t *c, int port, int cmd, int par, ethcam_reply_t *r)
{
	if (cmd_send(c, port, cmd, par) < 0)
		return -1;
	return cmd_wait(c->priv, cmd, r);
}

static int cmd_do_data(rcd_cam_t *c, int cmd, int par, const void *data, size_t len,
		       ethcam_reply_t *r)
{
	ethcam_t *e = c->priv;
	if (cmd_do(c, ETHCAM_PORT_CMD, cmd, par, r) < 0 || r->status != ETHCAM_ST_CONTINUE)
		return -1;
	if (rcd_send_all(e->cmd_fd, data, len, CMD_TIMEOUT_MS) < 0)
		return -1;
	return cmd_wait(e, cmd, r);
}

static int handshake(rcd_cam_t *c, int info[2][3])
{
	ethcam_reply_t r;
	bool h265 = c->codec == RSS_CODEC_H265;
	uint32_t sys[3] = {0, c->want_sub ? 1u : 0u, 0}; /* PullModeEn, CloneDisplayPathEn */

	/* par = the module's encoder buffer in seconds (0: its default, 3) */
	if (cmd_do_data(c, ETHCAM_CMD_SET_SYS_INFO, ((ethcam_t *)c->priv)->enc_buf_sec, sys,
			sizeof(sys), &r) < 0 ||
	    (r.status & 0xf) != 12) {
		rcd_error(c, "system info (9027) rejected");
		return -1;
	}
	if (cmd_do(c, ETHCAM_PORT_CMD, ETHCAM_CMD_FW_VERSION, 0, &r) == 0)
		rcd_set_fw(c, r.string);
	/* VCodec 0 = H.264, 1 = H.265; module type 4 = NT96562 */
	uint32_t codec[3] = {h265 ? 1u : 0u, 4, 0};
	if (cmd_do_data(c, ETHCAM_CMD_SET_CODEC, 0, codec, sizeof(codec), &r) < 0)
		RSS_WARN("camera%d: codec set (9032) failed", c->index);
	cmd_do(c, ETHCAM_PORT_CMD, ETHCAM_CMD_STARTUP, 0, &r);

	time_t now = time(NULL);
	struct tm tm;
	gmtime_r(&now, &tm);
	/* the module wants a full year and months 1-12 */
	int32_t t[11] = {tm.tm_sec,	tm.tm_min,	   tm.tm_hour, tm.tm_mday,
			 tm.tm_mon + 1, tm.tm_year + 1900, tm.tm_wday, tm.tm_yday};
	if (cmd_do_data(c, ETHCAM_CMD_SYNC_TIME, 0, t, sizeof(t), &r) < 0)
		RSS_WARN("camera%d: time sync (9014) failed", c->index);

	if (cmd_do(c, ETHCAM_PORT_REC, ETHCAM_CMD_GET_REC_INFO, 0, &r) < 0 || r.item_count < 7) {
		rcd_error(c, "no record stream info (9012)");
		return -1;
	}
	info[RCD_MAIN][0] = r.items[0];
	info[RCD_MAIN][1] = r.items[1];
	info[RCD_MAIN][2] = r.items[2];
	if (c->want_sub && cmd_do(c, ETHCAM_PORT_DISP, ETHCAM_CMD_GET_DISP_INFO, 0, &r) == 0 &&
	    r.item_count >= 8) {
		info[RCD_SUB][0] = r.items[4];
		info[RCD_SUB][1] = r.items[5];
		info[RCD_SUB][2] = r.items[3];
	}
	return 0;
}

static int data_read(rcd_cam_t *c, int idx)
{
	ethcam_t *e = c->priv;
	bool h265 = c->codec == RSS_CODEC_H265;
	ssize_t n = recv(e->fd[idx], e->buf[idx] + e->len[idx], RX_BUF - e->len[idx], 0);
	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
		rcd_error(c, "data connection closed");
		return -1;
	}
	if (n > 0)
		e->len[idx] += (size_t)n;

	size_t off = 0;
	for (;;) {
		ethcam_frame_t f;
		int used = ethcam_parse_frame(e->buf[idx] + off, e->len[idx] - off, h265, &f);
		if (used < 0) {
			rcd_error(c, "data stream out of sync");
			return -1;
		}
		if (used == 0)
			break;
		off += (size_t)used;
		if (!f.chk_ok) {
			rcd_drop(c, idx, f.ts_us);
			continue;
		}
		rss_iov_t iov[3];
		uint32_t k = 0;
		if (f.sps) {
			iov[k++] = (rss_iov_t){f.sps, f.sps_len};
			iov[k++] = (rss_iov_t){f.pps, f.pps_len};
		}
		iov[k++] = (rss_iov_t){f.body, f.body_len};
		rcd_publish(c, idx, iov, k, f.ts_us, f.key);
	}
	if (off) {
		memmove(e->buf[idx], e->buf[idx] + off, e->len[idx] - off);
		e->len[idx] -= off;
	}
	return 0;
}

static void close_all(rcd_cam_t *c)
{
	ethcam_t *e = c->priv;
	/* No stop command here: a command still in flight when its connection
	 * closes can wedge the module's command handler until a power cycle.
	 * The next session stops the streams itself, waiting for the reply. */
	for (int i = 0; i < 2; i++) {
		if (e->fd[i] >= 0)
			close(e->fd[i]);
		e->fd[i] = -1;
		e->len[i] = 0;
	}
	if (e->cmd_fd >= 0) {
		close(e->cmd_fd);
		e->cmd_fd = -1;
	}
	e->cmd_len = 0;
}

static int session(rcd_cam_t *c)
{
	ethcam_t *e = c->priv;
	ethcam_reply_t r;
	int info[2][3] = {{0}};
	int ret = -1;

	e->cmd_fd = rcd_tcp_connect(c->ip, ETHCAM_PORT_CMD, CMD_TIMEOUT_MS);
	if (e->cmd_fd < 0) {
		rcd_error(c, "no answer on the command port");
		goto out;
	}
	e->cmd_last_rx_us = rss_timestamp_us();
	if (handshake(c, info) < 0)
		goto out;

	int nst = c->want_sub && info[RCD_SUB][0] > 0 ? 2 : 1;
	for (int i = 0; i < nst; i++) {
		if (rcd_stream_open(c, i, c->codec, info[i][0], info[i][1], info[i][2]) < 0)
			goto out;
		e->fd[i] = rcd_tcp_connect(c->ip, data_port[i], CMD_TIMEOUT_MS);
		if (e->fd[i] < 0) {
			rcd_error(c, "no answer on data port %d", data_port[i]);
			goto out;
		}
	}
	/* Start from a stopped module whatever the last session left: a start on
	 * a stream the module still runs returns OK without re-arming its send
	 * flag, and nothing arrives until the module reboots itself. */
	if (cmd_do(c, ETHCAM_PORT_CMD, ETHCAM_CMD_STREAM_STOP, 1, &r) < 0) {
		rcd_error(c, "stream stop (9009) failed");
		goto out;
	}
	usleep(500000);
	for (int i = nst - 1; i >= 0; i--) { /* display first, as stock does */
		if (cmd_do(c, data_port[i], ETHCAM_CMD_STREAM_START, 0, &r) < 0) {
			rcd_error(c, "stream start (9008) failed");
			goto out;
		}
	}
	rcd_streaming(c);

	int64_t next_hb = rss_timestamp_us() + HEARTBEAT_US;
	while (rcd_should_run(c)) {
		struct pollfd p[3];
		p[0] = (struct pollfd){.fd = e->cmd_fd, .events = POLLIN};
		for (int i = 0; i < nst; i++)
			p[1 + i] = (struct pollfd){.fd = e->fd[i], .events = POLLIN};
		if (poll(p, (nfds_t)(1 + nst), 200) < 0 && errno != EINTR)
			goto out;

		if (p[0].revents & (POLLIN | POLLHUP | POLLERR)) {
			if (cmd_recv(e) < 0) {
				rcd_error(c, "command connection closed");
				goto out;
			}
			while (cmd_next(e, &r) > 0)
				;
		}
		for (int i = 0; i < nst; i++) {
			if ((p[1 + i].revents & (POLLIN | POLLHUP | POLLERR)) &&
			    data_read(c, i) < 0)
				goto out;
			if (rcd_keyframe_wanted(c, i) &&
			    cmd_send(c, data_port[i], ETHCAM_CMD_RESET_I, 0) < 0)
				goto out;
		}
		if (rcd_tick(c) < 0)
			goto out;

		int64_t now = rss_timestamp_us();
		if (now >= next_hb) {
			if (cmd_send(c, ETHCAM_PORT_CMD, ETHCAM_CMD_FW_VERSION, 0) < 0)
				goto out;
			next_hb = now + HEARTBEAT_US;
		}
		if (now - e->cmd_last_rx_us > CMD_DEAD_US) {
			rcd_error(c, "command connection silent");
			goto out;
		}
	}
	ret = 0;
out:
	close_all(c);
	return ret;
}

static int init(rcd_cam_t *c, rss_config_t *cfg, const char *section)
{
	ethcam_t *e = calloc(1, sizeof(*e));
	if (!e)
		return -1;
	/* a full-sensor (2592x1944) module only fits its encoder with 2 */
	e->enc_buf_sec = rss_config_get_int(cfg, section, "enc_buf_sec", 0);
	if (e->enc_buf_sec < 0 || e->enc_buf_sec > 20) {
		RSS_WARN("%s: enc_buf_sec must be 0..20, using the module default", section);
		e->enc_buf_sec = 0;
	}
	e->cmd_fd = e->fd[0] = e->fd[1] = -1;
	for (int i = 0; i < 2; i++) {
		e->buf[i] = malloc(RX_BUF);
		if (!e->buf[i]) {
			free(e->buf[0]);
			free(e);
			return -1;
		}
	}
	c->priv = e;
	return 0;
}

static void cleanup(rcd_cam_t *c)
{
	ethcam_t *e = c->priv;
	if (!e)
		return;
	free(e->buf[0]);
	free(e->buf[1]);
	free(e);
	c->priv = NULL;
}

const rcd_driver_t rcd_drv_ethcam = {
	.name = "ethcam",
	.init = init,
	.session = session,
	.cleanup = cleanup,
	/* after a stream stops, 9008 answers OK and sends nothing; a third
	 * session wedges the command handler */
	.one_session_per_boot = true,
};

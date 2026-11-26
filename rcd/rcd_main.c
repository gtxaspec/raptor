/*
 * rcd_main.c -- Raptor Camera Daemon
 *
 * Brings external cameras into raptor as extra sensors: a camera in slot N
 * publishes already-encoded video into sN_main / sN_sub, so RTSP, WebRTC
 * and recording pick it up like an on-board sensor. Frames carry our
 * monotonic clock (the camera's clock mapped onto it) and every ring
 * carries the monotonic->UTC mapping, as rvd's rings do.
 *
 * One thread per camera runs its driver's session (rcd.h). rcd also owns
 * the camera link: interface address, DHCP server with a static lease per
 * camera MAC, and the camera power GPIO.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <raptor_hal.h>

#include "rcd.h"
#include "rcd_ethcam.h"

#define RCD_SOCK	RSS_RUN_DIR "/rcd.sock"
#define RCD_DHCP_CONF	RSS_RUN_DIR "/rcd-udhcpd.conf"
#define RCD_DHCP_LEASES RSS_RUN_DIR "/rcd-udhcpd.leases"
#define RCD_MAX_CAMS	4
#define RCD_MAX_SLOT	2 /* consumers know s1_* and s2_* */
#define RCD_STALL_US	3000000
#define RCD_UTC_US	5000000
#define RCD_RETRY_US	2000000
#define RCD_POWER_AFTER 3 /* failed sessions in a row before a power cycle */

extern char **environ;

static const rcd_driver_t *const drivers[] = {&rcd_drv_ethcam};

enum { ST_IDLE, ST_CONNECTING, ST_STREAMING, ST_ERROR };
static const char *const state_name[] = {"idle", "connecting", "streaming", "error"};
static const char *const stream_name[] = {"main", "sub"};

typedef struct {
	volatile sig_atomic_t *running;
	rss_config_t *cfg;
	const char *config_path;
	rss_ctrl_t *ctrl;
	char iface[IFNAMSIZ];
	pid_t dhcp_pid;
	int power_gpio;
	pthread_mutex_t power_lock;
	rcd_cam_t cams[RCD_MAX_CAMS];
	int cam_count;
	int64_t next_utc[RCD_MAX_CAMS];
} rcd_state_t;

static rcd_state_t rs;

/* ── Shared socket helpers ── */

int rcd_tcp_connect(const char *ip, int port, int timeout_ms)
{
	struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
	if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1)
		return -1;
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0)
		return -1;
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 && errno != EINPROGRESS)
		goto fail;
	struct pollfd p = {.fd = fd, .events = POLLOUT};
	if (poll(&p, 1, timeout_ms) != 1)
		goto fail;
	int err = 0;
	socklen_t el = sizeof(err);
	if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err)
		goto fail;
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	return fd;
fail:
	close(fd);
	return -1;
}

int rcd_send_all(int fd, const void *data, size_t len, int timeout_ms)
{
	const uint8_t *p = data;
	while (len) {
		ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
		if (n > 0) {
			p += n;
			len -= (size_t)n;
			continue;
		}
		if (n < 0 && errno != EAGAIN && errno != EINTR)
			return -1;
		struct pollfd pf = {.fd = fd, .events = POLLOUT};
		if (poll(&pf, 1, timeout_ms) != 1)
			return -1;
	}
	return 0;
}

/* ── Core services for drivers ── */

bool rcd_should_run(rcd_cam_t *c)
{
	return rss_running(rs.running) && !atomic_load(&c->restart);
}

void rcd_set_fw(rcd_cam_t *c, const char *fw)
{
	pthread_mutex_lock(&c->info_lock);
	rss_strlcpy(c->fw, fw, sizeof(c->fw));
	pthread_mutex_unlock(&c->info_lock);
}

void rcd_error(rcd_cam_t *c, const char *fmt, ...)
{
	char msg[96];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	pthread_mutex_lock(&c->info_lock);
	rss_strlcpy(c->last_error, msg, sizeof(c->last_error));
	pthread_mutex_unlock(&c->info_lock);
	RSS_WARN("camera%d: %s", c->index, msg);
}

int rcd_stream_open(rcd_cam_t *c, int idx, int codec, int width, int height, int fps)
{
	rcd_stream_t *s = &c->st[idx];
	rss_ring_t *ring = s->ring;
	if (!ethcam_sane_size(width, height)) {
		rcd_error(c, "%s stream: unusable size %dx%d", stream_name[idx], width, height);
		return -1;
	}
	fps = ethcam_sane_fps(fps);
	if (!ring) {
		char name[24];
		snprintf(name, sizeof(name), "s%d_%s", c->slot, stream_name[idx]);
		ring = rss_ring_create(name, 64, (uint32_t)c->data_mb[idx] * 1024 * 1024);
		if (!ring) {
			rcd_error(c, "cannot create ring %s", name);
			return -1;
		}
		RSS_INFO("camera%d: ring %s (%d MB)", c->index, name, c->data_mb[idx]);
	}
	/* The status reply reads these from the control thread */
	pthread_mutex_lock(&c->info_lock);
	s->ring = ring;
	s->codec = codec;
	s->width = width;
	s->height = height;
	s->fps = fps;
	pthread_mutex_unlock(&c->info_lock);
	s->info_set = false;
	s->last_frame_us = rss_timestamp_us();
	s->open = true;
	RSS_INFO("camera%d: %s %dx%d@%d %s", c->index, stream_name[idx], width, height, s->fps,
		 codec == RSS_CODEC_H265 ? "h265" : "h264");
	return 0;
}

/* Profile and level from the SPS among the keyframe's NAL units */
static void sps_profile_level(const rss_iov_t *iov, uint32_t n, bool h265, uint8_t *profile,
			      uint8_t *level)
{
	for (uint32_t k = 0; k < n; k++) {
		const uint8_t *d = iov[k].data;
		uint32_t len = iov[k].length < 1024 ? iov[k].length : 1024;
		for (uint32_t i = 0; i + 20 < len; i++) {
			if (d[i] != 0 || d[i + 1] != 0 || d[i + 2] != 1)
				continue;
			const uint8_t *p = d + i + 3;
			if (!h265 && (p[0] & 0x1f) == 7) {
				*profile = p[1];
				*level = p[3];
				return;
			}
			/* H.265 SPS: 2-byte NAL header, 1 byte of ids, profile_tier_level */
			if (h265 && ((p[0] >> 1) & 0x3f) == 33) {
				*profile = p[3] & 0x1f;
				*level = p[14];
				return;
			}
		}
	}
}

static uint64_t period_us(const rcd_stream_t *s)
{
	return 1000000u / (uint64_t)s->fps;
}

void rcd_publish(rcd_cam_t *c, int idx, const rss_iov_t *iov, uint32_t n, uint64_t cam_ts_us,
		 bool key)
{
	rcd_stream_t *s = &c->st[idx];
	int64_t now = rss_timestamp_us();
	int64_t ts = now;
	uint32_t seq = RSS_SRC_SEQ_NONE;
	if (cam_ts_us != RCD_TS_LOCAL) {
		uint64_t eff;
		seq = rcd_seq(&s->seq, cam_ts_us, period_us(s), &eff);
		ts = rcd_tsmap(&s->tsmap, eff, now);
	}
	bool h265 = s->codec == RSS_CODEC_H265;

	if (!s->info_set && key) {
		uint8_t profile = 0, level = 0;
		sps_profile_level(iov, n, h265, &profile, &level);
		rss_ring_set_stream_info(s->ring, (uint32_t)(c->slot * 2 + idx), (uint32_t)s->codec,
					 (uint32_t)s->width, (uint32_t)s->height, (uint32_t)s->fps,
					 1, profile, level);
		s->info_set = true;
	}
	uint16_t nal = h265 ? (key ? RSS_NAL_H265_IDR : RSS_NAL_H265_SLICE)
			    : (key ? RSS_NAL_H264_IDR : RSS_NAL_H264_SLICE);
	rss_ring_publish_iov(s->ring, iov, n, ts, nal, key ? 1 : 0, seq);

	uint64_t bytes = 0;
	for (uint32_t k = 0; k < n; k++)
		bytes += iov[k].length;
	s->last_frame_us = now;
	atomic_fetch_add(&s->frames, 1);
	atomic_fetch_add(&s->bytes, bytes);
	if (key)
		atomic_fetch_add(&s->keyframes, 1);
}

void rcd_drop(rcd_cam_t *c, int idx, uint64_t cam_ts_us)
{
	rcd_stream_t *s = &c->st[idx];
	s->last_frame_us = rss_timestamp_us();
	atomic_fetch_add(&s->dropped, 1);
	uint64_t eff;
	rss_ring_report_drop(s->ring, cam_ts_us == RCD_TS_LOCAL
					      ? RSS_SRC_SEQ_NONE
					      : rcd_seq(&s->seq, cam_ts_us, period_us(s), &eff));
}

bool rcd_keyframe_wanted(rcd_cam_t *c, int idx)
{
	rcd_stream_t *s = &c->st[idx];
	/* the request is consumed either way, so a refused one does not linger */
	return s->open && rss_ring_check_idr(s->ring) && c->keyframe_requests;
}

void rcd_streaming(rcd_cam_t *c)
{
	atomic_store(&c->state, ST_STREAMING);
	atomic_fetch_add(&c->sessions, 1);
	pthread_mutex_lock(&c->info_lock);
	c->last_error[0] = '\0';
	pthread_mutex_unlock(&c->info_lock);
}

int rcd_tick(rcd_cam_t *c)
{
	int64_t now = rss_timestamp_us();
	for (int i = 0; i < 2; i++) {
		if (c->st[i].open && now - c->st[i].last_frame_us > RCD_STALL_US) {
			rcd_error(c, "%s stream: no frames for 3 s", stream_name[i]);
			return -1;
		}
	}
	int64_t *next = &rs.next_utc[c - rs.cams];
	if (now >= *next) {
		int64_t wall = rss_wallclock_us(), mono = rss_timestamp_us();
		uint8_t status = rss_ntp_synced() ? RSS_UTC_STATUS_LOCKED : RSS_UTC_STATUS_UNLOCKED;
		for (int i = 0; i < 2; i++) {
			if (c->st[i].ring)
				rss_ring_set_utc(c->st[i].ring, wall - mono, status);
		}
		*next = now + RCD_UTC_US;
	}
	return 0;
}

static int gpio_write(int gpio, int val);

/* Cut the camera supply for 3 s. It feeds every camera on the link, so
 * the lock keeps two failing cameras from cycling it at once. */
static void power_cycle(const char *why)
{
	if (rs.power_gpio < 0)
		return;
	pthread_mutex_lock(&rs.power_lock);
	RSS_WARN("power-cycling the cameras: %s", why);
	gpio_write(rs.power_gpio, 0);
	for (int i = 0; i < 30 && rss_running(rs.running); i++)
		usleep(100000);
	if (gpio_write(rs.power_gpio, 1) < 0)
		RSS_WARN("cannot switch camera power (gpio %d)", rs.power_gpio);
	pthread_mutex_unlock(&rs.power_lock);
}

static void *cam_thread(void *arg)
{
	rcd_cam_t *c = arg;
	int failed = 0;

	while (rss_running(rs.running)) {
		atomic_store(&c->restart, false);
		atomic_store(&c->state, ST_CONNECTING);
		uint32_t before = atomic_load(&c->sessions);
		int ret = c->driver->session(c);
		c->st[RCD_MAIN].open = c->st[RCD_SUB].open = false;
		if (!rss_running(rs.running))
			break;
		bool streamed = atomic_load(&c->sessions) != before;
		/* a camera that answers but never gets to streaming may be wedged */
		failed = !streamed && ret < 0 ? failed + 1 : 0;
		if (streamed && c->driver->one_session_per_boot) {
			if (rs.power_gpio < 0)
				rcd_error(c, "needs a power cycle to stream again (no power_gpio)");
			power_cycle("a camera needs a fresh boot to stream again");
		} else if (failed >= RCD_POWER_AFTER) {
			power_cycle("a camera did not come up");
			failed = 0;
		}
		atomic_store(&c->state, ret < 0 ? ST_ERROR : ST_CONNECTING);
		int64_t until = rss_timestamp_us() + RCD_RETRY_US;
		while (rss_running(rs.running) && !atomic_load(&c->restart) &&
		       rss_timestamp_us() < until)
			usleep(100000);
	}
	atomic_store(&c->state, ST_IDLE);
	return NULL;
}

/* ── Link: interface address, DHCP server, power ── */

static int set_iface_addr(const char *iface, const char *ip, const char *mask)
{
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	struct ifreq ifr = {0};
	rss_strlcpy(ifr.ifr_name, iface, sizeof(ifr.ifr_name));
	struct sockaddr_in *sa = (struct sockaddr_in *)&ifr.ifr_addr;
	sa->sin_family = AF_INET;
	int ret = -1;
	if (inet_pton(AF_INET, ip, &sa->sin_addr) != 1 || ioctl(fd, SIOCSIFADDR, &ifr) < 0)
		goto out;
	if (inet_pton(AF_INET, mask, &sa->sin_addr) != 1 || ioctl(fd, SIOCSIFNETMASK, &ifr) < 0)
		goto out;
	if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0)
		goto out;
	ifr.ifr_flags |= IFF_UP;
	ret = ioctl(fd, SIOCSIFFLAGS, &ifr);
out:
	close(fd);
	return ret;
}

static int sysfs_write(const char *path, const char *val)
{
	FILE *f = fopen(path, "w");
	if (!f)
		return -1;
	int ret = fputs(val, f) < 0 ? -1 : 0;
	if (fclose(f) != 0)
		ret = -1;
	return ret;
}

static int gpio_write(int gpio, int val)
{
	char path[64], v[16];
	snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", gpio);
	if (access(path, F_OK) != 0) {
		snprintf(v, sizeof(v), "%d", gpio);
		sysfs_write("/sys/class/gpio/export", v);
		snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", gpio);
		sysfs_write(path, "out");
		snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", gpio);
	}
	return sysfs_write(path, val ? "1" : "0");
}

static void dhcp_start(void)
{
	const char *start = rss_config_get_str(rs.cfg, "cameras", "dhcp_start", "192.168.0.12");
	const char *end = rss_config_get_str(rs.cfg, "cameras", "dhcp_end", "192.168.0.20");
	FILE *f = fopen(RCD_DHCP_CONF, "w");
	if (!f) {
		RSS_ERROR("cannot write %s: %m", RCD_DHCP_CONF);
		return;
	}
	fprintf(f, "interface %s\nstart %s\nend %s\nlease_file %s\noption subnet %s\n", rs.iface,
		start, end, RCD_DHCP_LEASES,
		rss_config_get_str(rs.cfg, "cameras", "netmask", "255.255.255.0"));
	for (int i = 0; i < rs.cam_count; i++) {
		if (rs.cams[i].mac[0])
			fprintf(f, "static_lease %s %s\n", rs.cams[i].mac, rs.cams[i].ip);
	}
	fclose(f);
	FILE *l = fopen(RCD_DHCP_LEASES, "a"); /* udhcpd will not create it */
	if (l)
		fclose(l);

	char *argv[] = {"udhcpd", "-f", RCD_DHCP_CONF, NULL};
	if (posix_spawnp(&rs.dhcp_pid, "udhcpd", NULL, NULL, argv, environ) != 0) {
		RSS_ERROR("cannot start udhcpd");
		rs.dhcp_pid = 0;
		return;
	}
	RSS_INFO("dhcp server on %s (%s-%s)", rs.iface, start, end);
}

static void dhcp_stop(void)
{
	if (rs.dhcp_pid > 0) {
		kill(rs.dhcp_pid, SIGTERM);
		waitpid(rs.dhcp_pid, NULL, 0);
		rs.dhcp_pid = 0;
	}
}

static void link_up(void)
{
	rss_strlcpy(rs.iface, rss_config_get_str(rs.cfg, "cameras", "iface", ""), sizeof(rs.iface));
	if (rs.iface[0]) {
		const char *ip = rss_config_get_str(rs.cfg, "cameras", "local_ip", "192.168.0.3");
		const char *mask =
			rss_config_get_str(rs.cfg, "cameras", "netmask", "255.255.255.0");
		if (set_iface_addr(rs.iface, ip, mask) < 0)
			RSS_WARN("cannot set %s on %s: %m", ip, rs.iface);
		if (rss_config_get_bool(rs.cfg, "cameras", "dhcp", true))
			dhcp_start();
	}

	rs.power_gpio = rss_config_get_int(rs.cfg, "cameras", "power_gpio", -1);
	if (rs.power_gpio < 0)
		return;
	/* a fresh boot clears whatever state a previous session left in a camera */
	if (rss_config_get_bool(rs.cfg, "cameras", "power_cycle", true))
		power_cycle("start");
	else if (gpio_write(rs.power_gpio, 1) < 0)
		RSS_WARN("cannot switch camera power (gpio %d)", rs.power_gpio);
}

/* ── Config ── */

static bool valid_mac(const char *m)
{
	unsigned b[6];
	return strlen(m) == 17 &&
	       sscanf(m, "%2x:%2x:%2x:%2x:%2x:%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6;
}

static const rcd_driver_t *find_driver(const char *type)
{
	for (size_t i = 0; i < sizeof(drivers) / sizeof(drivers[0]); i++) {
		if (strcmp(drivers[i]->name, type) == 0)
			return drivers[i];
	}
	return NULL;
}

static int load_camera(rcd_cam_t *c, int index)
{
	char sec[24];
	snprintf(sec, sizeof(sec), "camera%d", index);
	if (!rss_config_get_bool(rs.cfg, sec, "enabled", false))
		return -1;

	memset(c, 0, sizeof(*c));
	c->index = index;
	rss_strlcpy(c->section, sec, sizeof(c->section));
	const char *type = rss_config_get_str(rs.cfg, sec, "type", "ethcam");
	c->driver = find_driver(type);
	if (!c->driver) {
		RSS_ERROR("%s: unknown camera type '%s'", sec, type);
		return -1;
	}
	rss_strlcpy(c->ip, rss_config_get_str(rs.cfg, sec, "ip", ""), sizeof(c->ip));
	struct in_addr a;
	if (inet_pton(AF_INET, c->ip, &a) != 1) {
		RSS_ERROR("%s: bad ip '%s'", sec, c->ip);
		return -1;
	}
	const char *mac = rss_config_get_str(rs.cfg, sec, "mac", "");
	if (mac[0] && !valid_mac(mac)) {
		RSS_ERROR("%s: bad mac '%s'", sec, mac);
		return -1;
	}
	rss_strlcpy(c->mac, mac, sizeof(c->mac));
	c->slot = rss_config_get_int(rs.cfg, sec, "slot", index);
	if (c->slot < 1 || c->slot > RCD_MAX_SLOT) {
		RSS_ERROR("%s: slot must be 1..%d", sec, RCD_MAX_SLOT);
		return -1;
	}
	for (int k = 0; k < rs.cam_count; k++) {
		if (rs.cams[k].slot == c->slot) {
			RSS_ERROR("%s: slot %d already taken by camera%d", sec, c->slot,
				  rs.cams[k].index);
			return -1;
		}
	}
	/* H.265: the same picture as H.264 for less of the camera link */
	const char *codec = rss_config_get_str(rs.cfg, sec, "codec", "h265");
	c->codec = strcmp(codec, "h264") == 0 ? RSS_CODEC_H264 : RSS_CODEC_H265;
	c->want_sub = rss_config_get_bool(rs.cfg, sec, "sub", true);
	c->keyframe_requests = rss_config_get_bool(rs.cfg, sec, "keyframe_requests", true);
	c->data_mb[RCD_MAIN] = rss_config_get_int(rs.cfg, sec, "main_data_mb", 3);
	c->data_mb[RCD_SUB] = rss_config_get_int(rs.cfg, sec, "sub_data_mb", 1);
	pthread_mutex_init(&c->info_lock, NULL);
	if (c->driver->init && c->driver->init(c, rs.cfg, sec) < 0) {
		RSS_ERROR("%s: %s driver init failed", sec, c->driver->name);
		pthread_mutex_destroy(&c->info_lock);
		return -1;
	}
	return 0;
}

/* ── Control ── */

static cJSON *cam_json(rcd_cam_t *c)
{
	cJSON *o = cJSON_CreateObject();
	if (!o)
		return NULL;
	cJSON_AddNumberToObject(o, "camera", c->index);
	cJSON_AddStringToObject(o, "type", c->driver->name);
	cJSON_AddStringToObject(o, "ip", c->ip);
	cJSON_AddStringToObject(o, "mac", c->mac);
	cJSON_AddNumberToObject(o, "slot", c->slot);
	cJSON_AddStringToObject(o, "state", state_name[atomic_load(&c->state)]);
	cJSON_AddStringToObject(o, "codec", c->codec == RSS_CODEC_H265 ? "h265" : "h264");
	cJSON_AddNumberToObject(o, "sessions", atomic_load(&c->sessions));
	pthread_mutex_lock(&c->info_lock);
	cJSON_AddStringToObject(o, "fw", c->fw);
	cJSON_AddStringToObject(o, "error", c->last_error);
	pthread_mutex_unlock(&c->info_lock);
	cJSON *arr = cJSON_AddArrayToObject(o, "streams");
	for (int i = 0; i < 2; i++) {
		rcd_stream_t *s = &c->st[i];
		pthread_mutex_lock(&c->info_lock);
		bool have = s->ring != NULL;
		int width = s->width, height = s->height, fps = s->fps;
		pthread_mutex_unlock(&c->info_lock);
		if (!have)
			continue;
		cJSON *so = cJSON_CreateObject();
		if (!so)
			continue;
		cJSON_AddStringToObject(so, "name", stream_name[i]);
		cJSON_AddNumberToObject(so, "width", width);
		cJSON_AddNumberToObject(so, "height", height);
		cJSON_AddNumberToObject(so, "fps", fps);
		cJSON_AddNumberToObject(so, "frames", atomic_load(&s->frames));
		cJSON_AddNumberToObject(so, "keyframes", atomic_load(&s->keyframes));
		cJSON_AddNumberToObject(so, "bytes", (double)atomic_load(&s->bytes));
		cJSON_AddNumberToObject(so, "dropped", atomic_load(&s->dropped));
		cJSON_AddNumberToObject(so, "camera_restarts", atomic_load(&s->seq.resets));
		cJSON_AddItemToArray(arr, so);
	}
	return o;
}

static int ctrl_handler(const char *cmd_json, char *resp_buf, int resp_buf_size, void *userdata)
{
	(void)userdata;
	int common =
		rss_ctrl_handle_common(cmd_json, resp_buf, resp_buf_size, rs.cfg, rs.config_path);
	if (common >= 0)
		return common;

	char cmd[32] = "";
	rss_json_get_str(cmd_json, "cmd", cmd, sizeof(cmd));

	if (strcmp(cmd, "reconnect") == 0) {
		int idx = 0;
		rss_json_get_int(cmd_json, "value", &idx);
		bool hit = false;
		for (int i = 0; i < rs.cam_count; i++) {
			if (idx == 0 || rs.cams[i].index == idx) {
				atomic_store(&rs.cams[i].restart, true);
				hit = true;
			}
		}
		return hit ? rss_ctrl_resp_ok(resp_buf, resp_buf_size)
			   : rss_ctrl_resp_error(resp_buf, resp_buf_size, "no such camera");
	}

	cJSON *o = cJSON_CreateObject();
	if (!o)
		return rss_ctrl_resp_error(resp_buf, resp_buf_size, "out of memory");
	cJSON_AddStringToObject(o, "status", "ok");
	cJSON_AddStringToObject(o, "iface", rs.iface);
	cJSON_AddBoolToObject(o, "dhcp", rs.dhcp_pid > 0);
	cJSON *arr = cJSON_AddArrayToObject(o, "cameras");
	for (int i = 0; i < rs.cam_count; i++) {
		cJSON *co = cam_json(&rs.cams[i]);
		if (co)
			cJSON_AddItemToArray(arr, co);
	}
	return rss_ctrl_resp_json(resp_buf, resp_buf_size, o);
}

/* ── Main ── */

int main(int argc, char **argv)
{
	rss_daemon_ctx_t dctx;
	int ret = rss_daemon_init(&dctx, "rcd", argc, argv, NULL);
	if (ret != 0)
		return ret < 0 ? 1 : 0;

	rs.running = dctx.running;
	rs.power_gpio = -1;
	pthread_mutex_init(&rs.power_lock, NULL);
	rs.cfg = dctx.cfg;
	rs.config_path = dctx.config_path;

	if (!rss_config_get_bool(rs.cfg, "cameras", "enabled", false)) {
		RSS_INFO("external cameras disabled");
		goto out;
	}
	for (int i = 1; i <= RCD_MAX_CAMS; i++) {
		if (load_camera(&rs.cams[rs.cam_count], i) == 0)
			rs.cam_count++;
	}
	if (rs.cam_count == 0) {
		RSS_INFO("no external cameras configured");
		goto out;
	}

	rss_mkdir_p(RSS_RUN_DIR);
	rs.ctrl = rss_ctrl_listen(RCD_SOCK);
	int ctrl_fd = rs.ctrl ? rss_ctrl_get_fd(rs.ctrl) : -1;

	link_up();

	for (int i = 0; i < rs.cam_count; i++) {
		rcd_cam_t *c = &rs.cams[i];
		if (pthread_create(&c->thread, NULL, cam_thread, c) == 0)
			c->thread_started = true;
		else
			RSS_ERROR("camera%d: cannot start thread", c->index);
	}

	while (*rs.running) {
		struct pollfd p = {.fd = ctrl_fd, .events = POLLIN};
		int n = poll(&p, ctrl_fd >= 0 ? 1 : 0, 500);
		if (n > 0 && (p.revents & POLLIN))
			rss_ctrl_accept_and_handle(rs.ctrl, ctrl_handler, NULL);
		if (rs.dhcp_pid > 0 && waitpid(rs.dhcp_pid, NULL, WNOHANG) == rs.dhcp_pid) {
			RSS_WARN("udhcpd exited, restarting");
			rs.dhcp_pid = 0;
			dhcp_start();
		}
	}

	for (int i = 0; i < rs.cam_count; i++) {
		if (rs.cams[i].thread_started)
			pthread_join(rs.cams[i].thread, NULL);
	}

out:
	dhcp_stop();
	if (rs.ctrl)
		rss_ctrl_destroy(rs.ctrl);
	for (int i = 0; i < rs.cam_count; i++) {
		rcd_cam_t *c = &rs.cams[i];
		if (c->driver->cleanup)
			c->driver->cleanup(c);
		for (int k = 0; k < 2; k++) {
			if (c->st[k].ring)
				rss_ring_destroy(c->st[k].ring);
		}
		pthread_mutex_destroy(&c->info_lock);
	}
	rss_config_free(dctx.cfg);
	rss_daemon_cleanup("rcd");
	return 0;
}

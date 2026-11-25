/*
 * rtd_main.c -- RTD (Raptor Telematics Daemon)
 *
 * Reads a GPS receiver (NMEA) and an accelerometer, both described in
 * /etc/thingino.json, and once a second:
 *   - publishes an rtd_record_t on the "telematics" ring (rmr records it)
 *   - feeds rod template variables for the OSD
 * An acceleration spike beyond [telematics] impact_g starts an rmr event
 * clip and raises %event%.
 */

#include "rtd.h"

#include <rss_common.h>
#include <rss_ipc.h>
#include <cJSON.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/rtc.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define THINGINO_JSON	   "/etc/thingino.json"
#define ROD_SOCK	   RSS_RUN_DIR "/rod.sock"
#define RMR_SOCK	   RSS_RUN_DIR "/rmr.sock"
#define RTD_SOCK	   RSS_RUN_DIR "/rtd.sock"

#define GPS_SILENT_US	   3000000 /* receiver considered gone */
#define EPOCH_SETTLE_US	   150000  /* let the rest of the burst after RMC arrive */
#define EPOCH_MAX_US	   1200000 /* publish on our own clock past this */
#define WARMUP_US	   2000000 /* resting vector settles before impacts count */
#define ROD_RETRY_US	   10000000
#define CLOCK_TOLERANCE_US 2000000 /* NTP and GPS agree within this: no tug of war */
#define CLOCK_CHECK_US	   600000000

typedef enum { UNIT_KMH, UNIT_MPH } speed_unit_t;

typedef struct {
	volatile sig_atomic_t *running;
	rss_config_t *cfg;
	const char *config_path;
	rss_ctrl_t *ctrl;
	rss_ring_t *ring;

	/* GPS */
	char gps_dev[64];
	int gps_baud;
	int gps_fd;
	FILE *replay;
	char line[128];
	size_t line_len;
	rtd_fix_t fix;
	int64_t last_sentence_us;
	int64_t epoch_due_us; /* 0 = none pending */
	int64_t last_publish_us;
	int64_t rmc_us; /* when the last RMC arrived */

	/* Clock */
	bool set_clock;
	int64_t next_clock_check_us;

	/* Accelerometer */
	rtd_accel_t accel;
	bool have_accel;
	int accel_period_us;
	int64_t next_sample_us;
	int64_t accel_start_us;
	float rest[3];
	float peak[3];
	float peak_dev;
	bool impact;
	bool spike_pending;
	bool manual_event;
	float spike[3];
	float spike_dev;

	/* Impact handling */
	float impact_g;
	bool impact_clip;
	int impact_clip_sec;
	int64_t clip_stop_us; /* 0 = no clip held */

	/* OSD */
	bool osd;
	speed_unit_t unit;
	int64_t rod_retry_us;
	char vars[16][2][32]; /* last value sent per name */
	int nvars;

	rtd_record_t last;

	bool sim;
	rtd_sim_t simst;
} rtd_state_t;

/* ── Hardware description ── */

static int json_int(cJSON *o, const char *key, int def)
{
	cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
	if (cJSON_IsNumber(v))
		return v->valueint;
	if (cJSON_IsString(v) && v->valuestring[0])
		return (int)strtol(v->valuestring, NULL, 0);
	return def;
}

/* "gps": {"uart": "/dev/ttyS1", "baud": 9600}
 * "gsensor": {"model": "da221", "i2c": 2, "addr": 39} */
static void load_hw(rtd_state_t *st, int *acc_bus, int *acc_addr)
{
	*acc_bus = -1;
	*acc_addr = 0;
	FILE *f = fopen(THINGINO_JSON, "r");
	if (!f)
		return;
	char buf[8192];
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';
	cJSON *root = cJSON_Parse(buf);
	if (!root) {
		RSS_WARN("%s does not parse", THINGINO_JSON);
		return;
	}
	cJSON *gps = cJSON_GetObjectItemCaseSensitive(root, "gps");
	cJSON *uart = cJSON_GetObjectItemCaseSensitive(gps, "uart");
	if (cJSON_IsString(uart))
		rss_strlcpy(st->gps_dev, uart->valuestring, sizeof(st->gps_dev));
	st->gps_baud = json_int(gps, "baud", 9600);

	cJSON *gs = cJSON_GetObjectItemCaseSensitive(root, "gsensor");
	if (gs) {
		cJSON *model = cJSON_GetObjectItemCaseSensitive(gs, "model");
		if (cJSON_IsString(model) && strcmp(model->valuestring, "da221") != 0)
			RSS_WARN("g-sensor model %s unsupported", model->valuestring);
		else {
			*acc_bus = json_int(gs, "i2c", -1);
			*acc_addr = json_int(gs, "addr", 0);
		}
	}
	cJSON_Delete(root);
}

/* ── GPS input ── */

static speed_t baud_const(int baud)
{
	switch (baud) {
	case 4800:
		return B4800;
	case 19200:
		return B19200;
	case 38400:
		return B38400;
	case 57600:
		return B57600;
	case 115200:
		return B115200;
	default:
		return B9600;
	}
}

static int gps_open(rtd_state_t *st)
{
	int fd = open(st->gps_dev, O_RDONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		RSS_ERROR("gps: %s: %m", st->gps_dev);
		return -1;
	}
	struct termios t;
	if (tcgetattr(fd, &t) == 0) {
		cfmakeraw(&t);
		t.c_cflag |= CLOCAL | CREAD;
		cfsetispeed(&t, baud_const(st->gps_baud));
		cfsetospeed(&t, baud_const(st->gps_baud));
		tcsetattr(fd, TCSANOW, &t);
	}
	st->gps_fd = fd;
	RSS_INFO("gps: %s at %d baud", st->gps_dev, st->gps_baud);
	return 0;
}

static void gps_line(rtd_state_t *st, const char *line, int64_t now)
{
	int r = rtd_nmea_parse(line, &st->fix);
	if (r == RTD_NMEA_BAD)
		return;
	st->last_sentence_us = now;
	if (r == RTD_NMEA_RMC) {
		st->rmc_us = now;
		if (!st->epoch_due_us)
			st->epoch_due_us = now + EPOCH_SETTLE_US;
	}
}

static void gps_read(rtd_state_t *st, int64_t now)
{
	char buf[256];
	ssize_t n;
	while ((n = read(st->gps_fd, buf, sizeof(buf))) > 0) {
		for (ssize_t i = 0; i < n; i++) {
			char c = buf[i];
			if (c == '\n' || c == '\r') {
				if (st->line_len) {
					st->line[st->line_len] = '\0';
					gps_line(st, st->line, now);
				}
				st->line_len = 0;
			} else if (st->line_len < sizeof(st->line) - 1) {
				st->line[st->line_len++] = c;
			} else {
				st->line_len = 0;
			}
		}
	}
}

/* One epoch of a recorded NMEA log: everything up to and including
 * the next RMC. Loops at end of file. */
static void replay_epoch(rtd_state_t *st, int64_t now)
{
	char line[256];
	for (int wrapped = 0; wrapped < 2;) {
		if (!fgets(line, sizeof(line), st->replay)) {
			rewind(st->replay);
			wrapped++;
			continue;
		}
		int r = rtd_nmea_parse(line, &st->fix);
		if (r == RTD_NMEA_BAD)
			continue;
		st->last_sentence_us = now;
		if (r == RTD_NMEA_RMC)
			return;
	}
}

/* One epoch of the simulated drive, through the same parser as a receiver */
static void sim_epoch(rtd_state_t *st, int64_t now)
{
	char buf[256];
	rtd_sim_nmea(&st->simst, now, rss_wallclock_us(), buf, sizeof(buf));
	for (char *line = strtok(buf, "\r\n"); line; line = strtok(NULL, "\r\n"))
		gps_line(st, line, now);
}

/* ── Accelerometer ── */

static void accel_sample(rtd_state_t *st, int64_t now)
{
	float g[3];
	if (st->sim)
		rtd_sim_accel(&st->simst, now, g);
	else if (rtd_accel_read(&st->accel, g) < 0)
		return;
	if (st->accel_start_us == 0) {
		st->accel_start_us = now;
		memcpy(st->rest, g, sizeof(g));
	}
	float d[3], dev = 0.0f;
	for (int i = 0; i < 3; i++) {
		d[i] = g[i] - st->rest[i];
		dev += d[i] * d[i];
	}
	dev = sqrtf(dev);

	/* A real impact lasts tens of milliseconds, several samples. One
	 * sample alone over the threshold is a corrupted read (seen as a
	 * 1.8 g sign-flip glitch at rest under load): hold it until the
	 * next sample confirms it, and never let it move the rest vector. */
	bool over = st->impact_g > 0 && dev > st->impact_g;
	if (over && !st->spike_pending) {
		st->spike_pending = true;
		st->spike_dev = dev;
		memcpy(st->spike, g, sizeof(g));
		return;
	}
	if (st->spike_pending) {
		st->spike_pending = false;
		if (over) {
			if (st->spike_dev >= st->peak_dev) {
				st->peak_dev = st->spike_dev;
				memcpy(st->peak, st->spike, sizeof(st->spike));
			}
			if (now - st->accel_start_us > WARMUP_US)
				st->impact = true;
		} else {
			RSS_DEBUG("g-sensor: dropped a lone %.2f g sample", st->spike_dev);
		}
	}
	if (dev >= st->peak_dev) {
		st->peak_dev = dev;
		memcpy(st->peak, g, sizeof(g));
	}
	if (over)
		return;
	/* ~2 s time constant: follows mounting angle and slopes, not bumps */
	float alpha = (float)st->accel_period_us / 2e6f;
	for (int i = 0; i < 3; i++)
		st->rest[i] += alpha * d[i];
}

/* ── Outputs ── */

static void rod_var(rtd_state_t *st, const char *name, const char *value, int64_t now)
{
	int i;
	for (i = 0; i < st->nvars; i++)
		if (strcmp(st->vars[i][0], name) == 0)
			break;
	if (i < st->nvars && strcmp(st->vars[i][1], value) == 0)
		return;
	if (now < st->rod_retry_us)
		return;

	char cmd[160], resp[128];
	cJSON *j = cJSON_CreateObject();
	if (!j)
		return;
	cJSON_AddStringToObject(j, "cmd", "set-var");
	cJSON_AddStringToObject(j, "name", name);
	cJSON_AddStringToObject(j, "value", value);
	bool ok = cJSON_PrintPreallocated(j, cmd, sizeof(cmd), 0);
	cJSON_Delete(j);
	if (!ok)
		return;
	if (rss_ctrl_send_command(ROD_SOCK, cmd, resp, sizeof(resp), 200) < 0) {
		st->rod_retry_us = now + ROD_RETRY_US;
		st->nvars = 0; /* rod may have restarted: resend everything */
		return;
	}
	if (i == st->nvars) {
		if (st->nvars >= (int)(sizeof(st->vars) / sizeof(st->vars[0])))
			return;
		rss_strlcpy(st->vars[i][0], name, sizeof(st->vars[i][0]));
		st->nvars++;
	}
	rss_strlcpy(st->vars[i][1], value, sizeof(st->vars[i][1]));
}

static void update_osd(rtd_state_t *st, const rtd_record_t *r, int64_t now)
{
	char v[32];
	bool fix = r->flags & RTD_F_FIX;
	/* read each time: set-speed-unit changes it live */
	st->unit =
		strcmp(rss_config_get_str(st->cfg, "telematics", "speed_unit", "kmh"), "mph") == 0
			? UNIT_MPH
			: UNIT_KMH;
	double speed = r->speed_kn * (st->unit == UNIT_MPH ? 1.150779 : 1.852);

	snprintf(v, sizeof(v), "%d", fix ? (int)lround(speed) : 0);
	rod_var(st, "speed", v, now);
	rod_var(st, "unit", st->unit == UNIT_MPH ? "mph" : "Km/h", now);
	snprintf(v, sizeof(v), "%c:%.3f", r->lat < 0 ? 'S' : 'N', fix ? fabs(r->lat) : 0.0);
	rod_var(st, "lat", v, now);
	snprintf(v, sizeof(v), "%c:%.3f", r->lon < 0 ? 'W' : 'E', fix ? fabs(r->lon) : 0.0);
	rod_var(st, "lon", v, now);
	snprintf(v, sizeof(v), "%d", fix ? (int)lround(r->alt_m) : 0);
	rod_var(st, "alt", v, now);
	snprintf(v, sizeof(v), "%03d", fix ? (int)lround(r->course_deg) % 360 : 0);
	rod_var(st, "heading", v, now);
	snprintf(v, sizeof(v), "%d", r->sats);
	rod_var(st, "sats", v, now);
	rod_var(st, "gps", !(r->flags & RTD_F_GPS) ? "NO GPS" : fix ? "GPS" : "NO FIX", now);

	if (r->flags & RTD_F_ACCEL) {
		snprintf(v, sizeof(v), "%+.2f", r->acc_g[0]);
		rod_var(st, "gx", v, now);
		snprintf(v, sizeof(v), "%+.2f", r->acc_g[1]);
		rod_var(st, "gy", v, now);
		snprintf(v, sizeof(v), "%+.2f", r->acc_g[2]);
		rod_var(st, "gz", v, now);
		snprintf(v, sizeof(v), "%.2f", r->acc_dev_g);
		rod_var(st, "g", v, now);
	}
	rod_var(st, "event", st->clip_stop_us ? "SOS" : "", now);
}

static void rmr_clip(bool start)
{
	char resp[128];
	if (rss_ctrl_cmd(RMR_SOCK, start ? "start" : "stop", resp, sizeof(resp), 1000) < 0)
		RSS_WARN("impact clip %s: rmr not answering", start ? "start" : "stop");
}

/* RMC carries the time of the second it starts; it arrives a fraction
 * of a second later, well inside the tolerance. The RTC is written
 * too, so the next boot starts with this time (battery-backed RTC). */
static void gps_set_clock(rtd_state_t *st, const rtd_record_t *r, int64_t now)
{
	if (!st->set_clock || st->replay || st->sim || now < st->next_clock_check_us)
		return;
	if (!(r->flags & RTD_F_TIME) || !(r->flags & RTD_F_FIX) || r->sats < 4)
		return;
	st->next_clock_check_us = now + CLOCK_CHECK_US;

	int64_t gps_now = r->utc_us + (now - st->rmc_us);
	int64_t off = gps_now - rss_wallclock_us();
	if (off > -CLOCK_TOLERANCE_US && off < CLOCK_TOLERANCE_US)
		return;

	struct timespec ts = {.tv_sec = gps_now / 1000000, .tv_nsec = (gps_now % 1000000) * 1000};
	if (clock_settime(CLOCK_REALTIME, &ts) < 0) {
		RSS_WARN("gps: cannot set the clock: %m");
		return;
	}
	RSS_INFO("gps: clock was %+.1f s off, set from GPS", (double)off / 1e6);

	int fd = open("/dev/rtc0", O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	struct tm tm;
	time_t t = ts.tv_sec;
	gmtime_r(&t, &tm);
	struct rtc_time rt = {
		.tm_sec = tm.tm_sec,
		.tm_min = tm.tm_min,
		.tm_hour = tm.tm_hour,
		.tm_mday = tm.tm_mday,
		.tm_mon = tm.tm_mon,
		.tm_year = tm.tm_year,
	};
	if (ioctl(fd, RTC_SET_TIME, &rt) < 0)
		RSS_WARN("gps: cannot set the RTC: %m");
	close(fd);
}

static void publish(rtd_state_t *st, int64_t now)
{
	rtd_record_t r = {.version = RTD_REC_VERSION, .ts_us = now};
	const rtd_fix_t *f = &st->fix;

	if (st->last_sentence_us && now - st->last_sentence_us < GPS_SILENT_US) {
		r.flags |= RTD_F_GPS;
		r.utc_us = rtd_fix_utc_us(f);
		if (r.utc_us)
			r.flags |= RTD_F_TIME;
		if (f->valid) {
			r.flags |= RTD_F_FIX;
			r.lat = f->lat;
			r.lon = f->lon;
			r.alt_m = (float)f->alt_m;
			r.speed_kn = (float)f->speed_kn;
			r.course_deg = (float)f->course_deg;
			r.hdop = (float)f->hdop;
		}
		r.sats = (uint8_t)f->sats;
		r.quality = (uint8_t)f->quality;
	}
	if (st->have_accel && st->accel_start_us) {
		r.flags |= RTD_F_ACCEL;
		memcpy(r.acc_g, st->peak, sizeof(r.acc_g));
		r.acc_dev_g = st->peak_dev;
	}
	if (st->impact) {
		r.flags |= RTD_F_IMPACT;
		if (st->manual_event)
			RSS_INFO("event (manual)");
		else
			RSS_INFO("impact: %.2f g", st->peak_dev);
		st->manual_event = false;
		if (st->impact_clip) {
			if (!st->clip_stop_us)
				rmr_clip(true);
			st->clip_stop_us = now + (int64_t)st->impact_clip_sec * 1000000;
		}
	}
	st->impact = false;
	st->peak_dev = 0.0f;
	memcpy(st->peak, st->rest, sizeof(st->peak));

	gps_set_clock(st, &r, now);
	rss_ring_publish(st->ring, (const uint8_t *)&r, sizeof(r), now, 0, 0, RSS_SRC_SEQ_NONE);
	st->last = r;
	st->last_publish_us = now;
	st->epoch_due_us = 0;

	if (st->osd)
		update_osd(st, &r, now);
}

/* ── Control ── */

static int ctrl_handler(const char *cmd_json, char *resp_buf, int resp_buf_size, void *userdata)
{
	rtd_state_t *st = userdata;
	int common =
		rss_ctrl_handle_common(cmd_json, resp_buf, resp_buf_size, st->cfg, st->config_path);
	if (common >= 0)
		return common;

	char cmd[32] = "", val[16] = "";
	rss_json_get_str(cmd_json, "cmd", cmd, sizeof(cmd));
	if (strcmp(cmd, "set-speed-unit") == 0) {
		rss_json_get_str(cmd_json, "value", val, sizeof(val));
		if (strcmp(val, "kmh") != 0 && strcmp(val, "mph") != 0)
			return rss_ctrl_resp_error(resp_buf, resp_buf_size, "kmh or mph");
		rss_config_set_str(st->cfg, "telematics", "speed_unit", val);
		return rss_ctrl_resp_ok(resp_buf, resp_buf_size);
	}

	if (strcmp(cmd, "set-impact-g") == 0) {
		rss_json_get_str(cmd_json, "value", val, sizeof(val));
		char *end;
		float g = strtof(val, &end);
		if (end == val || g < 0 || g > 16)
			return rss_ctrl_resp_error(resp_buf, resp_buf_size, "0..16 g");
		st->impact_g = g;
		return rss_ctrl_resp_ok(resp_buf, resp_buf_size);
	}

	/* A manual event (the dashcam's protect button): handled like an
	 * impact at the next epoch, clip and all. */
	if (strcmp(cmd, "event") == 0) {
		st->impact = true;
		st->manual_event = true;
		return rss_ctrl_resp_ok(resp_buf, resp_buf_size);
	}

	const rtd_record_t *r = &st->last;
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "status", "ok");
	cJSON_AddBoolToObject(o, "gps", r->flags & RTD_F_GPS);
	cJSON_AddBoolToObject(o, "fix", r->flags & RTD_F_FIX);
	cJSON_AddNumberToObject(o, "utc", (double)(r->utc_us / 1000000));
	cJSON_AddNumberToObject(o, "lat", r->lat);
	cJSON_AddNumberToObject(o, "lon", r->lon);
	cJSON_AddNumberToObject(o, "alt", r->alt_m);
	cJSON_AddNumberToObject(o, "speed_kn", r->speed_kn);
	cJSON_AddNumberToObject(o, "course", r->course_deg);
	cJSON_AddNumberToObject(o, "sats", r->sats);
	cJSON_AddNumberToObject(o, "hdop", r->hdop);
	if (r->flags & RTD_F_ACCEL) {
		cJSON *a = cJSON_AddArrayToObject(o, "accel");
		for (int i = 0; i < 3; i++)
			cJSON_AddItemToArray(a, cJSON_CreateNumber(r->acc_g[i]));
		cJSON_AddNumberToObject(o, "accel_dev", r->acc_dev_g);
	}
	cJSON_AddBoolToObject(o, "event", st->clip_stop_us != 0);
	if (st->sim) {
		cJSON_AddBoolToObject(o, "sim", true);
		cJSON_AddNumberToObject(o, "sim_impacts", st->simst.impacts);
	}
	return rss_ctrl_resp_json(resp_buf, resp_buf_size, o);
}

/* ── Main ── */

int main(int argc, char **argv)
{
	rss_daemon_ctx_t dctx;
	int ret = rss_daemon_init(&dctx, "rtd", argc, argv, NULL);
	if (ret != 0)
		return ret < 0 ? 1 : 0;

	rtd_state_t st = {.gps_fd = -1, .accel = {.fd = -1}};
	st.running = dctx.running;
	st.cfg = dctx.cfg;
	st.config_path = dctx.config_path;

	int acc_bus, acc_addr;
	load_hw(&st, &acc_bus, &acc_addr);

	const char *s = rss_config_get_str(st.cfg, "telematics", "gps_device", "");
	if (s[0])
		rss_strlcpy(st.gps_dev, s, sizeof(st.gps_dev));
	const char *replay = rss_config_get_str(st.cfg, "telematics", "gps_replay", "");
	st.impact_g = strtof(rss_config_get_str(st.cfg, "telematics", "impact_g", "1.5"), NULL);
	st.impact_clip = rss_config_get_bool(st.cfg, "telematics", "impact_clip", true);
	st.impact_clip_sec = rss_config_get_int(st.cfg, "telematics", "impact_clip_sec", 30);
	st.osd = rss_config_get_bool(st.cfg, "telematics", "osd", true);
	st.set_clock = rss_config_get_bool(st.cfg, "telematics", "set_clock", true);
	int hz = rss_config_get_int(st.cfg, "telematics", "accel_hz", 50);
	st.accel_period_us = 1000000 / (hz < 1 ? 1 : hz > 400 ? 400 : hz);

	st.sim = rss_config_get_bool(st.cfg, "telematics", "sim", false);
	if (st.sim) {
		rtd_sim_cfg_t sc = {
			.seed = (uint32_t)rss_config_get_int(st.cfg, "telematics", "sim_seed", 1),
			.lat = strtod(
				rss_config_get_str(st.cfg, "telematics", "sim_lat", "51.4779"),
				NULL),
			.lon = strtod(
				rss_config_get_str(st.cfg, "telematics", "sim_lon", "-0.0015"),
				NULL),
			.fix_after_s =
				rss_config_get_int(st.cfg, "telematics", "sim_fix_after_s", 30),
			.dropout_every_s = rss_config_get_int(st.cfg, "telematics",
							      "sim_dropout_every_s", 300),
			.impact_every_s =
				rss_config_get_int(st.cfg, "telematics", "sim_impact_every_s", 600),
			.impact_g = strtof(
				rss_config_get_str(st.cfg, "telematics", "sim_impact_g", "2.5"),
				NULL),
		};
		rtd_sim_init(&st.simst, &sc, rss_timestamp_us());
		st.have_accel = true;
		RSS_INFO("simulated drive: fix after %d s, an impact every %d s", sc.fix_after_s,
			 sc.impact_every_s);
	} else if (replay[0]) {
		st.replay = fopen(replay, "r");
		if (!st.replay)
			RSS_ERROR("gps replay %s: %m", replay);
		else
			RSS_INFO("gps: replaying %s", replay);
	} else if (st.gps_dev[0]) {
		gps_open(&st);
	}
	if (!st.sim && acc_bus >= 0 && acc_addr > 0)
		st.have_accel = rtd_accel_open(&st.accel, acc_bus, acc_addr) == 0;

	if (st.gps_fd < 0 && !st.replay && !st.sim && !st.have_accel) {
		RSS_INFO("no GPS or g-sensor on this camera");
		goto out;
	}

	st.ring = rss_ring_create(RTD_RING, 16, 16 * sizeof(rtd_record_t));
	if (!st.ring) {
		RSS_ERROR("cannot create the %s ring", RTD_RING);
		goto out;
	}

	rss_mkdir_p(RSS_RUN_DIR);
	st.ctrl = rss_ctrl_listen(RTD_SOCK);
	int ctrl_fd = st.ctrl ? rss_ctrl_get_fd(st.ctrl) : -1;

	int64_t now = rss_timestamp_us();
	st.next_sample_us = now;
	st.last_publish_us = now;

	while (*st.running) {
		now = rss_timestamp_us();

		int64_t wake = st.last_publish_us + (st.replay || st.sim ? 1000000 : EPOCH_MAX_US);
		if (st.epoch_due_us && st.epoch_due_us < wake)
			wake = st.epoch_due_us;
		if (st.have_accel && st.next_sample_us < wake)
			wake = st.next_sample_us;
		int timeout = wake > now ? (int)((wake - now + 999) / 1000) : 0;

		struct pollfd pfd[2];
		int n = 0;
		if (st.gps_fd >= 0)
			pfd[n++] = (struct pollfd){.fd = st.gps_fd, .events = POLLIN};
		if (ctrl_fd >= 0)
			pfd[n++] = (struct pollfd){.fd = ctrl_fd, .events = POLLIN};
		if (poll(pfd, (nfds_t)n, timeout) < 0 && errno != EINTR)
			break;
		now = rss_timestamp_us();

		for (int i = 0; i < n; i++) {
			if (!(pfd[i].revents & POLLIN))
				continue;
			if (pfd[i].fd == st.gps_fd)
				gps_read(&st, now);
			else
				rss_ctrl_accept_and_handle(st.ctrl, ctrl_handler, &st);
		}

		if (st.have_accel && now >= st.next_sample_us) {
			accel_sample(&st, now);
			st.next_sample_us += st.accel_period_us;
			if (st.next_sample_us < now) /* fell behind: don't burst */
				st.next_sample_us = now + st.accel_period_us;
		}

		if (st.replay || st.sim) {
			if (now - st.last_publish_us >= 1000000) {
				if (st.sim)
					sim_epoch(&st, now);
				else
					replay_epoch(&st, now);
				publish(&st, now);
			}
		} else if ((st.epoch_due_us && now >= st.epoch_due_us) ||
			   now - st.last_publish_us >= EPOCH_MAX_US) {
			publish(&st, now);
		}

		if (st.clip_stop_us && now >= st.clip_stop_us) {
			rmr_clip(false);
			st.clip_stop_us = 0;
		}
	}

	if (st.clip_stop_us)
		rmr_clip(false);

out:
	if (st.ctrl)
		rss_ctrl_destroy(st.ctrl);
	if (st.ring)
		rss_ring_destroy(st.ring);
	if (st.gps_fd >= 0)
		close(st.gps_fd);
	if (st.replay)
		fclose(st.replay);
	rtd_accel_close(&st.accel);
	rss_config_free(dctx.cfg);
	rss_daemon_cleanup("rtd");
	return 0;
}

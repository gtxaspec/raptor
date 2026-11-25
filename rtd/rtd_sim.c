/*
 * rtd_sim.c -- a simulated drive: NMEA and g-sensor samples
 *
 * For benches, QEMU and soak tests: rtd runs its real parser and impact
 * filter on what this produces, so the rest of the system (rmr clips, GPS
 * records, OSD, the web UI) sees a car, not a camera on a shelf.
 */
#include "rtd.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define M_PER_DEG_LAT 111320.0
#define MPS_TO_KN     1.943844
#define CYCLE_S	      120 /* pull away, cruise, brake, wait at a light */
#define DROPOUT_S     8	  /* a tunnel */

static float frand(rtd_sim_t *s) /* [0, 1) */
{
	uint32_t x = s->rng;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	s->rng = x;
	return (float)(x >> 8) / 16777216.0f;
}

void rtd_sim_init(rtd_sim_t *s, const rtd_sim_cfg_t *cfg, int64_t now_us)
{
	memset(s, 0, sizeof(*s));
	s->cfg = *cfg;
	s->rng = cfg->seed ? cfg->seed : 1;
	s->start_us = now_us;
	s->last_us = now_us;
	s->lat = cfg->lat;
	s->lon = cfg->lon;
	s->alt_m = 35.0;
	s->rest[0] = 0.0f;
	s->rest[1] = 0.10f; /* a dash mount leans back a little */
	s->rest[2] = 0.99f;
	if (cfg->impact_every_s > 0)
		s->next_impact_us = now_us + (int64_t)cfg->impact_every_s * 1000000;
}

/* Target speed over the drive cycle, m/s */
static double cycle_speed(double t)
{
	double c = fmod(t, CYCLE_S);
	if (c < 12)
		return 14.0 * c / 12.0;
	if (c < 80)
		return 14.0;
	if (c < 92)
		return 14.0 * (92.0 - c) / 12.0;
	return 0.0;
}

static void advance(rtd_sim_t *s, int64_t now_us)
{
	double dt = (double)(now_us - s->last_us) / 1e6;
	s->last_us = now_us;
	if (dt <= 0)
		return;
	double t = (double)(now_us - s->start_us) / 1e6 - s->cfg.fix_after_s;
	if (t < 0)
		return; /* parked until the first fix */
	s->speed_mps = cycle_speed(t);
	double c = fmod(t, CYCLE_S);
	if (c >= 30 && c < 45)
		s->heading += 6.0 * dt; /* a long bend */
	if (c >= 92 && c < 93)
		s->heading += 90.0 * dt; /* turn at the light */
	s->heading = fmod(s->heading + 360.0, 360.0);
	double d = s->speed_mps * dt, h = s->heading * M_PI / 180.0;
	s->lat += d * cos(h) / M_PER_DEG_LAT;
	s->lon += d * sin(h) / (M_PER_DEG_LAT * cos(s->lat * M_PI / 180.0));
}

static int nmea_put(char *buf, size_t len, const char *body)
{
	unsigned sum = 0;
	for (const char *p = body; *p; p++)
		sum ^= (unsigned char)*p;
	return snprintf(buf, len, "$%s*%02X\r\n", body, sum);
}

static void coord(char *out, size_t len, double v, int deg_digits, char pos, char neg)
{
	double a = fabs(v);
	int deg = (int)a;
	snprintf(out, len, "%0*d%07.4f,%c", deg_digits, deg, (a - deg) * 60.0, v >= 0 ? pos : neg);
}

bool rtd_sim_has_fix(const rtd_sim_t *s, int64_t now_us)
{
	double t = (double)(now_us - s->start_us) / 1e6;
	if (t < s->cfg.fix_after_s)
		return false;
	if (s->cfg.dropout_every_s > 0) {
		double c = fmod(t - s->cfg.fix_after_s, s->cfg.dropout_every_s);
		if (c >= s->cfg.dropout_every_s - DROPOUT_S)
			return false;
	}
	return true;
}

int rtd_sim_nmea(rtd_sim_t *s, int64_t now_us, int64_t utc_us, char *buf, size_t len)
{
	advance(s, now_us);
	bool fix = rtd_sim_has_fix(s, now_us);
	double t = (double)(now_us - s->start_us) / 1e6;

	/* Before its first fix a receiver keeps time but not always the date:
	 * the bench unit's said 2026-05-13 for a while. */
	time_t secs = (time_t)(utc_us / 1000000);
	if (t < s->cfg.fix_after_s / 2.0)
		secs -= 196 * 86400;
	struct tm tm;
	gmtime_r(&secs, &tm);
	char hms[48], dmy[40];
	snprintf(hms, sizeof(hms), "%02d%02d%02d.00", tm.tm_hour, tm.tm_min, tm.tm_sec);
	snprintf(dmy, sizeof(dmy), "%02d%02d%02d", tm.tm_mday, tm.tm_mon + 1, tm.tm_year % 100);

	int sats = fix ? 7 + (int)(frand(s) * 5) : (t < s->cfg.fix_after_s ? 0 : 2);
	double hdop = fix ? 0.8 + frand(s) * 0.8 : 0.0;
	char lat[40], lon[40], body[192];
	coord(lat, sizeof(lat), s->lat, 2, 'N', 'S');
	coord(lon, sizeof(lon), s->lon, 3, 'E', 'W');

	int n = 0;
	if (fix)
		snprintf(body, sizeof(body), "GPGGA,%s,%s,%s,1,%02d,%.2f,%.1f,M,0.0,M,,", hms, lat,
			 lon, sats, hdop, s->alt_m);
	else
		snprintf(body, sizeof(body), "GPGGA,%s,,,,,0,%02d,,,,,,,", hms, sats);
	n += nmea_put(buf + n, len - (size_t)n, body);
	if (fix)
		snprintf(body, sizeof(body), "GPRMC,%s,A,%s,%s,%.2f,%.1f,%s,,,A", hms, lat, lon,
			 s->speed_mps * MPS_TO_KN, s->heading, dmy);
	else
		snprintf(body, sizeof(body), "GPRMC,%s,V,,,,,,,%s,,,N", hms, dmy);
	if ((size_t)n < len)
		n += nmea_put(buf + n, len - (size_t)n, body);
	return n;
}

void rtd_sim_accel(rtd_sim_t *s, int64_t now_us, float g[3])
{
	for (int i = 0; i < 3; i++)
		g[i] = s->rest[i] + (frand(s) - 0.5f) * 0.04f;
	if (s->speed_mps > 1.0 && frand(s) < 0.01f)
		g[2] += 0.25f; /* a pothole: well under any impact threshold */

	if (s->next_impact_us && now_us >= s->next_impact_us) {
		s->impact_left = RTD_SIM_IMPACT_SAMPLES;
		s->impacts++;
		s->next_impact_us = now_us + (int64_t)s->cfg.impact_every_s * 1000000;
	}
	if (s->impact_left > 0) {
		s->impact_left--;
		g[0] += s->cfg.impact_g; /* rear-ended: a shove forward */
	}
}

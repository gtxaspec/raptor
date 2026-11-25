#include "greatest.h"
#include "rtd.h"

#include <math.h>
#include <string.h>

#define T0   1000000000LL
#define UTC0 1795600000000000LL /* 2026-11-25 */
#define SEC  1000000LL

static const rtd_sim_cfg_t cfg = {
	.seed = 7,
	.lat = 51.4779,
	.lon = -0.0015,
	.fix_after_s = 30,
	.dropout_every_s = 300,
	.impact_every_s = 600,
	.impact_g = 2.5f,
};

/* Feed epoch t (seconds after start) through the real parser */
static int epoch(rtd_sim_t *s, int t, rtd_fix_t *fix)
{
	char buf[256];
	rtd_sim_nmea(s, T0 + t * SEC, UTC0 + t * SEC, buf, sizeof(buf));
	int rmc = 0;
	for (char *line = strtok(buf, "\r\n"); line; line = strtok(NULL, "\r\n")) {
		int r = rtd_nmea_parse(line, fix);
		if (r == RTD_NMEA_BAD)
			return -1;
		rmc += r == RTD_NMEA_RMC;
	}
	return rmc;
}

TEST rtd_sim_cold_start_then_fix(void)
{
	rtd_sim_t s;
	rtd_fix_t fix = {0};
	rtd_sim_init(&s, &cfg, T0);
	for (int t = 0; t < 30; t++) {
		ASSERT_EQ(1, epoch(&s, t, &fix)); /* every sentence checksums */
		ASSERTm("no fix during the cold start", !fix.valid);
		ASSERT_EQ(0, fix.quality);
	}
	ASSERT_EQ(1, epoch(&s, 30, &fix));
	ASSERTm("a fix once the cold start is over", fix.valid);
	ASSERT(fix.sats >= 7 && fix.hdop > 0.5);
	ASSERT_IN_RANGE(51.4779, fix.lat, 1e-4);
	PASS();
}

/* A receiver before its fix reports a wrong date; the fix brings the right one */
TEST rtd_sim_wrong_date_before_fix(void)
{
	rtd_sim_t s;
	rtd_fix_t fix = {0};
	rtd_sim_init(&s, &cfg, T0);
	epoch(&s, 1, &fix);
	int64_t early = rtd_fix_utc_us(&fix);
	ASSERTm("date off by months", llabs(early - (UTC0 + SEC)) > 100 * 86400 * SEC);
	for (int t = 2; t <= 31; t++)
		epoch(&s, t, &fix);
	ASSERT_IN_RANGE((double)(UTC0 + 31 * SEC), (double)rtd_fix_utc_us(&fix), 1e6);
	PASS();
}

TEST rtd_sim_drives(void)
{
	rtd_sim_t s;
	rtd_fix_t fix = {0};
	rtd_sim_init(&s, &cfg, T0);
	for (int t = 0; t <= 31; t++)
		epoch(&s, t, &fix);
	double lat0 = fix.lat, lon0 = fix.lon;
	double top_kn = 0;
	for (int t = 32; t <= 150; t++) {
		epoch(&s, t, &fix);
		if (fix.speed_kn > top_kn)
			top_kn = fix.speed_kn;
	}
	double dn = (fix.lat - lat0) * 111320.0;
	double de = (fix.lon - lon0) * 111320.0 * cos(lat0 * M_PI / 180.0);
	ASSERTm("moved over 500 m", sqrt(dn * dn + de * de) > 500.0);
	ASSERT_IN_RANGE(27.2, top_kn, 0.2); /* 14 m/s cruise */
	PASS();
}

TEST rtd_sim_drops_the_fix_now_and_then(void)
{
	rtd_sim_t s;
	rtd_sim_init(&s, &cfg, T0);
	int lost = 0;
	for (int t = 31; t < 31 + 300; t++)
		lost += !rtd_sim_has_fix(&s, T0 + t * SEC);
	ASSERT(lost >= 5 && lost <= 10);
	PASS();
}

static float dev(const float g[3], const float r[3])
{
	float d = 0;
	for (int i = 0; i < 3; i++)
		d += (g[i] - r[i]) * (g[i] - r[i]);
	return sqrtf(d);
}

/* Ten minutes of driving at 50 Hz: no sample anywhere near an impact,
 * then one impact of consecutive samples, as the filter requires. */
TEST rtd_sim_impacts_only_when_due(void)
{
	rtd_sim_t s;
	rtd_sim_init(&s, &cfg, T0);
	float g[3], worst = 0;
	int64_t now = T0;
	char buf[256];
	for (; now < T0 + 600 * SEC - 20000; now += 20000) {
		if ((now - T0) % SEC == 0)
			rtd_sim_nmea(&s, now, UTC0 + (now - T0), buf, sizeof(buf));
		rtd_sim_accel(&s, now, g);
		float d = dev(g, s.rest);
		if (d > worst)
			worst = d;
	}
	ASSERTm("road noise and potholes stay under 0.5 g", worst < 0.5f);
	ASSERT_EQ(0, s.impacts);

	int over = 0, run = 0, best_run = 0;
	for (int i = 0; i < 50; i++, now += 20000) {
		rtd_sim_accel(&s, now, g);
		if (dev(g, s.rest) > 1.5f) {
			over++;
			run++;
			best_run = run > best_run ? run : best_run;
		} else {
			run = 0;
		}
	}
	ASSERT_EQ(1, s.impacts);
	ASSERT_EQ(RTD_SIM_IMPACT_SAMPLES, over);
	ASSERT_EQ(RTD_SIM_IMPACT_SAMPLES, best_run);
	PASS();
}

TEST rtd_sim_is_repeatable(void)
{
	rtd_sim_t a, b;
	rtd_sim_init(&a, &cfg, T0);
	rtd_sim_init(&b, &cfg, T0);
	char ba[256], bb[256];
	for (int t = 0; t < 120; t++) {
		int na = rtd_sim_nmea(&a, T0 + t * SEC, UTC0 + t * SEC, ba, sizeof(ba));
		int nb = rtd_sim_nmea(&b, T0 + t * SEC, UTC0 + t * SEC, bb, sizeof(bb));
		ASSERT_EQ(na, nb);
		ASSERT_MEM_EQ(ba, bb, (size_t)na);
	}
	PASS();
}

SUITE(rtd_sim_suite)
{
	RUN_TEST(rtd_sim_cold_start_then_fix);
	RUN_TEST(rtd_sim_wrong_date_before_fix);
	RUN_TEST(rtd_sim_drives);
	RUN_TEST(rtd_sim_drops_the_fix_now_and_then);
	RUN_TEST(rtd_sim_impacts_only_when_due);
	RUN_TEST(rtd_sim_is_repeatable);
}

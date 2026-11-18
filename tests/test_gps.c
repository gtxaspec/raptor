#include "greatest.h"
#include "rmr_gps.h"
#include "rmr_mux.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *g_out;
static uint32_t g_out_len;

static int out_write(const void *buf, uint32_t len, void *ctx)
{
	(void)ctx;
	uint8_t *p = realloc(g_out, g_out_len + len);
	if (!p)
		return -1;
	g_out = p;
	memcpy(g_out + g_out_len, buf, len);
	g_out_len += len;
	return 0;
}

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint32_t le32(const uint8_t *p)
{
	return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static float lef(const uint8_t *p)
{
	uint32_t v = le32(p);
	float f;
	memcpy(&f, &v, 4);
	return f;
}

TEST nmea_rmc_gga(void)
{
	rtd_fix_t f = {0};
	ASSERT_EQ(RTD_NMEA_GGA,
		  rtd_nmea_parse(
			  "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47", &f));
	ASSERT_EQ(
		RTD_NMEA_RMC,
		rtd_nmea_parse("$GNRMC,170000.000,A,3403.1320,N,11814.6220,W,10.00,45.0,181126,,,A*"
			       "56\r\n",
			       &f));
	ASSERT(f.valid);
	ASSERT(fabs(f.lat - (34 + 3.132 / 60)) < 1e-9);
	ASSERT(fabs(f.lon + (118 + 14.622 / 60)) < 1e-9);
	ASSERT_EQ(8, f.sats);
	ASSERT_EQ(2026, f.year);
	ASSERT_EQ(1795021200000000LL, rtd_fix_utc_us(&f));
	PASS();
}

TEST nmea_rejects(void)
{
	rtd_fix_t f = {0};
	/* bad checksum, missing checksum, no fix */
	ASSERT_EQ(RTD_NMEA_BAD,
		  rtd_nmea_parse(
			  "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*48", &f));
	ASSERT_EQ(RTD_NMEA_BAD, rtd_nmea_parse("$GPRMC,003543.840,V,,,,,0.00,,060180,,,N", &f));
	ASSERT_EQ(RTD_NMEA_RMC, rtd_nmea_parse("$GPRMC,003543.840,V,,,,,0.00,,060180,,,N*51", &f));
	ASSERT_FALSE(f.valid);
	/* the receiver's 1980 power-on date is not a time */
	ASSERT_EQ(0, rtd_fix_utc_us(&f));
	PASS();
}

TEST gps_box_layout(void)
{
	rtd_record_t r = {
		.flags = RTD_F_GPS | RTD_F_FIX | RTD_F_TIME | RTD_F_ACCEL,
		.utc_us = 1795021200000000LL + 5000000, /* 2026-11-18 17:00:05 */
		.lat = 34.0522,
		.lon = -118.2437,
		.speed_kn = 30.0f,
		.course_deg = 90.0f,
		.acc_g = {0.5f, -0.25f, 1.0f},
	};
	uint8_t b[RMR_GPS_BOX_SIZE];
	rmr_gps_build(b, &r);
	ASSERT_EQ(RMR_GPS_BOX_SIZE, be32(b));
	ASSERT_MEM_EQ("freeGPS ", b + 4, 8);
	ASSERT_EQ(17u, le32(b + 0x10));
	ASSERT_EQ(5u, le32(b + 0x18));
	ASSERT_EQ(26u, le32(b + 0x1c));
	ASSERT_EQ(11u, le32(b + 0x20));
	ASSERT_MEM_EQ("ANW", b + 0x28, 3);
	ASSERT(fabsf(lef(b + 0x2c) - 3403.132f) < 0.01f);
	ASSERT(fabsf(lef(b + 0x30) - 11814.622f) < 0.01f);
	ASSERT_EQ(128u, le32(b + 0x3c));
	ASSERT_EQ((uint32_t)-64, le32(b + 0x40));
	ASSERT_MEM_EQ("RTDx", b + 0x80, 4);
	PASS();
}

TEST gps_index_after_mvhd(void)
{
	g_out_len = 0;
	rmr_mux_t *m = rmr_mux_create(out_write, NULL);
	rmr_audio_params_t ap = {
		.codec = RMR_AUDIO_L16, .sample_rate = 16000, .channels = 1, .bits_per_sample = 16};
	rmr_mux_set_audio(m, &ap);
	ASSERT_EQ(0, rmr_mux_enable_gps(m, 4));
	ASSERT_EQ(0, rmr_mux_start(m));

	/* moov's first child is mvhd, the second the reserved index */
	uint32_t moov = be32(g_out);
	ASSERT_MEM_EQ("moov", g_out + moov + 4, 4);
	uint32_t mvhd = moov + 8;
	uint32_t gps = mvhd + be32(g_out + mvhd);
	ASSERT_MEM_EQ("gps ", g_out + gps + 4, 4);
	ASSERT_EQ(16u + 8 * 4, be32(g_out + gps));

	for (uint32_t i = 0; i < 6; i++) /* two past capacity are dropped */
		rmr_mux_gps_add(m, 1000 + i * 256);
	uint8_t *idx;
	uint32_t len;
	uint64_t off;
	ASSERT_EQ(0, rmr_mux_gps_index(m, RMR_GPS_BOX_SIZE, &idx, &len, &off));
	ASSERT_EQ((uint64_t)gps + 8, off);
	ASSERT_EQ(8u + 8 * 4, len);
	ASSERT_EQ(0x101u, be32(idx));
	ASSERT_EQ(4u, be32(idx + 4));
	ASSERT_EQ(1000u + 3 * 256, be32(idx + 8 + 8 * 3));
	ASSERT_EQ(256u, be32(idx + 12 + 8 * 3));
	free(idx);
	rmr_mux_destroy(m);
	free(g_out);
	g_out = NULL;
	PASS();
}

SUITE(gps_suite)
{
	RUN_TEST(nmea_rmc_gga);
	RUN_TEST(nmea_rejects);
	RUN_TEST(gps_box_layout);
	RUN_TEST(gps_index_after_mvhd);
}

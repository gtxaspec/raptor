/*
 * rmr_gps.c -- GPS and g-sensor records in recordings
 */

#include "rmr_gps.h"

#include <math.h>
#include <string.h>
#include <time.h>

static void le32(uint8_t *p, uint32_t v)
{
	p[0] = v & 0xff;
	p[1] = (v >> 8) & 0xff;
	p[2] = (v >> 16) & 0xff;
	p[3] = (v >> 24) & 0xff;
}

static void lef32(uint8_t *p, float f)
{
	uint32_t v;
	memcpy(&v, &f, 4);
	le32(p, v);
}

static void lef64(uint8_t *p, double d)
{
	uint64_t v;
	memcpy(&v, &d, 8);
	le32(p, (uint32_t)v);
	le32(p + 4, (uint32_t)(v >> 32));
}

static float nmea_coord(double deg)
{
	double a = fabs(deg);
	int d = (int)a;
	return (float)(d * 100 + (a - d) * 60.0);
}

void rmr_gps_build(uint8_t box[RMR_GPS_BOX_SIZE], const rtd_record_t *r)
{
	memset(box, 0, RMR_GPS_BOX_SIZE);
	box[2] = RMR_GPS_BOX_SIZE >> 8;
	memcpy(box + 4, "freeGPS ", 8);
	le32(box + 0x0c, 0x2c); /* stock: the 0x10..0x3c fields */

	if (r->flags & RTD_F_TIME) {
		time_t t = (time_t)(r->utc_us / 1000000);
		struct tm tm;
		gmtime_r(&t, &tm);
		le32(box + 0x10, (uint32_t)tm.tm_hour);
		le32(box + 0x14, (uint32_t)tm.tm_min);
		le32(box + 0x18, (uint32_t)tm.tm_sec);
		le32(box + 0x1c, (uint32_t)(tm.tm_year + 1900 - 2000));
		le32(box + 0x20, (uint32_t)(tm.tm_mon + 1));
		le32(box + 0x24, (uint32_t)tm.tm_mday);
	}

	bool fix = r->flags & RTD_F_FIX;
	box[0x28] = fix ? 'A' : 'V';
	box[0x29] = r->lat < 0 ? 'S' : 'N';
	box[0x2a] = r->lon < 0 ? 'W' : 'E';
	if (fix) {
		lef32(box + 0x2c, nmea_coord(r->lat));
		lef32(box + 0x30, nmea_coord(r->lon));
		lef32(box + 0x34, r->speed_kn);
		lef32(box + 0x38, r->course_deg);
	}
	/* zeros here would read as "no accelerometer" */
	if (r->flags & RTD_F_ACCEL)
		for (int i = 0; i < 3; i++)
			le32(box + 0x3c + 4 * i, (uint32_t)(int32_t)lrintf(r->acc_g[i] * 256.0f));

	uint8_t *x = box + 0x80;
	memcpy(x, "RTDx", 4);
	x[4] = 1; /* extension version */
	x[5] = (uint8_t)r->flags;
	x[6] = r->sats;
	x[7] = r->quality;
	le32(x + 0x08, (uint32_t)r->utc_us);
	le32(x + 0x0c, (uint32_t)((uint64_t)r->utc_us >> 32));
	lef64(x + 0x10, r->lat);
	lef64(x + 0x18, r->lon);
	lef32(x + 0x20, r->alt_m);
	lef32(x + 0x24, r->speed_kn);
	lef32(x + 0x28, r->course_deg);
	lef32(x + 0x2c, r->hdop);
	for (int i = 0; i < 3; i++)
		lef32(x + 0x30 + 4 * i, r->acc_g[i]);
	lef32(x + 0x3c, r->acc_dev_g);
}

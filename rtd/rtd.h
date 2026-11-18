/*
 * rtd.h -- RTD (Raptor Telemetry Daemon) records
 *
 * rtd publishes one record per GPS epoch (1 Hz, or once a second on
 * its own clock while the receiver is silent) on the "telemetry" ring.
 * rmr embeds the records in recordings; anything else may read them.
 */

#ifndef RTD_H
#define RTD_H

#include <stdbool.h>
#include <stdint.h>

#define RTD_RING	"telemetry"
#define RTD_REC_VERSION 1

#define RTD_F_GPS	0x01 /* receiver talking */
#define RTD_F_FIX	0x02 /* position valid */
#define RTD_F_TIME	0x04 /* utc_us valid */
#define RTD_F_ACCEL	0x08 /* acc_* valid */
#define RTD_F_IMPACT	0x10 /* impact threshold crossed this epoch */

typedef struct {
	uint16_t version;
	uint16_t flags;
	uint8_t sats;
	uint8_t quality; /* GGA fix quality */
	uint16_t reserved;
	int64_t ts_us;	/* rss_timestamp_us() of the epoch */
	int64_t utc_us; /* receiver UTC */
	double lat;	/* degrees, + = north */
	double lon;	/* degrees, + = east */
	float alt_m;
	float speed_kn;
	float course_deg;
	float hdop;
	float acc_g[3];	 /* the sample furthest from rest this epoch */
	float acc_dev_g; /* its distance from the resting vector */
} rtd_record_t;

/* ── NMEA ── */

#define RTD_NMEA_BAD   -1
#define RTD_NMEA_OTHER 0
#define RTD_NMEA_RMC   1
#define RTD_NMEA_GGA   2

typedef struct {
	bool valid; /* RMC status A */
	bool have_time;
	bool have_date;
	int year, mon, day, hour, min;
	double sec;
	double lat, lon;
	double speed_kn, course_deg;
	int quality, sats;
	double hdop, alt_m;
} rtd_fix_t;

/* Parse one sentence (with or without the CR/LF) into fix. Fields a
 * sentence does not carry are left alone, so RMC and GGA accumulate. */
int rtd_nmea_parse(const char *line, rtd_fix_t *fix);

/* UTC microseconds from the fix's date and time, or 0 without both. */
int64_t rtd_fix_utc_us(const rtd_fix_t *fix);

/* ── Accelerometer ── */

typedef struct {
	int fd;
	int addr;
	float scale; /* g per LSB of the left-justified 16-bit sample */
} rtd_accel_t;

int rtd_accel_open(rtd_accel_t *a, int bus, int addr);
int rtd_accel_read(rtd_accel_t *a, float g[3]);
void rtd_accel_close(rtd_accel_t *a);

#endif /* RTD_H */

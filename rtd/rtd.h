/*
 * rtd.h -- RTD (Raptor Telematics Daemon) records
 *
 * rtd publishes one record per GPS epoch (1 Hz, or once a second on
 * its own clock while the receiver is silent) on the "telematics" ring.
 * rmr embeds the records in recordings; anything else may read them.
 */

#ifndef RTD_H
#define RTD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RTD_RING	"telematics"
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

/* ── Simulated drive ([telematics] sim = true) ── */

#define RTD_SIM_IMPACT_SAMPLES 4 /* 80 ms at 50 Hz: the filter wants two in a row */

typedef struct {
	uint32_t seed;
	double lat, lon;     /* where the drive starts */
	int fix_after_s;     /* cold start: no fix before this */
	int dropout_every_s; /* fix lost for a few seconds once per period, 0 = never */
	int impact_every_s;  /* 0 = no impacts */
	float impact_g;	     /* size of a simulated impact */
} rtd_sim_cfg_t;

typedef struct {
	rtd_sim_cfg_t cfg;
	uint32_t rng;
	int64_t start_us, last_us;
	double lat, lon, alt_m, heading, speed_mps;
	float rest[3];
	int64_t next_impact_us; /* 0 = no impacts */
	int impact_left;	/* samples left in the current impact */
	int impacts;		/* started so far */
} rtd_sim_t;

void rtd_sim_init(rtd_sim_t *s, const rtd_sim_cfg_t *cfg, int64_t now_us);
/* One epoch of NMEA (GGA then RMC, CRLF-terminated) into buf; returns its
 * length. now_us drives the drive, utc_us is the time the receiver reports. */
int rtd_sim_nmea(rtd_sim_t *s, int64_t now_us, int64_t utc_us, char *buf, size_t len);
bool rtd_sim_has_fix(const rtd_sim_t *s, int64_t now_us);
/* One g-sensor sample, at the configured rate */
void rtd_sim_accel(rtd_sim_t *s, int64_t now_us, float g[3]);

#endif /* RTD_H */

/*
 * rtd_nmea.c -- NMEA 0183 RMC/GGA parsing
 */

#include "rtd.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NMEA_MAX_FIELDS 24

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

/* ddmm.mmmm (or dddmm.mmmm) plus hemisphere to signed degrees */
static bool parse_coord(const char *v, const char *hemi, double *out)
{
	if (!v[0] || !hemi[0])
		return false;
	char *end;
	double raw = strtod(v, &end);
	if (end == v)
		return false;
	int deg = (int)(raw / 100.0);
	double d = deg + (raw - deg * 100.0) / 60.0;
	if (hemi[0] == 'S' || hemi[0] == 'W')
		d = -d;
	*out = d;
	return true;
}

static bool parse_time(const char *v, rtd_fix_t *fix)
{
	if (strlen(v) < 6)
		return false;
	for (int i = 0; i < 6; i++)
		if (v[i] < '0' || v[i] > '9')
			return false;
	fix->hour = (v[0] - '0') * 10 + (v[1] - '0');
	fix->min = (v[2] - '0') * 10 + (v[3] - '0');
	fix->sec = strtod(v + 4, NULL);
	fix->have_time = true;
	return true;
}

static bool parse_date(const char *v, rtd_fix_t *fix)
{
	if (strlen(v) != 6)
		return false;
	for (int i = 0; i < 6; i++)
		if (v[i] < '0' || v[i] > '9')
			return false;
	fix->day = (v[0] - '0') * 10 + (v[1] - '0');
	fix->mon = (v[2] - '0') * 10 + (v[3] - '0');
	/* Receivers that never saw the sky report 1980 epochs: 06 01 80 */
	int yy = (v[4] - '0') * 10 + (v[5] - '0');
	fix->year = yy >= 80 ? 1900 + yy : 2000 + yy;
	fix->have_date = true;
	return true;
}

int rtd_nmea_parse(const char *line, rtd_fix_t *fix)
{
	char buf[128];
	if (line[0] != '$')
		return RTD_NMEA_BAD;
	size_t len = strcspn(line, "\r\n");
	if (len >= sizeof(buf) || len < 7)
		return RTD_NMEA_BAD;
	memcpy(buf, line, len);
	buf[len] = '\0';

	char *star = strchr(buf, '*');
	if (!star || star[1] == '\0' || star[2] == '\0')
		return RTD_NMEA_BAD;
	int hi = hexval(star[1]), lo = hexval(star[2]);
	if (hi < 0 || lo < 0)
		return RTD_NMEA_BAD;
	unsigned sum = 0;
	for (const char *p = buf + 1; p < star; p++)
		sum ^= (unsigned char)*p;
	if (sum != (unsigned)(hi << 4 | lo))
		return RTD_NMEA_BAD;
	*star = '\0';

	/* Split keeping empty fields (strtok would merge them) */
	char *f[NMEA_MAX_FIELDS];
	int n = 0;
	char *p = buf + 1;
	while (n < NMEA_MAX_FIELDS) {
		f[n++] = p;
		char *c = strchr(p, ',');
		if (!c)
			break;
		*c = '\0';
		p = c + 1;
	}
	if (strlen(f[0]) != 5)
		return RTD_NMEA_OTHER;
	const char *type = f[0] + 2; /* skip the talker (GP, GN, GL, ...) */

	if (strcmp(type, "RMC") == 0 && n >= 10) {
		parse_time(f[1], fix);
		fix->valid = f[2][0] == 'A';
		double lat, lon;
		if (parse_coord(f[3], f[4], &lat) && parse_coord(f[5], f[6], &lon)) {
			fix->lat = lat;
			fix->lon = lon;
		}
		fix->speed_kn = f[7][0] ? strtod(f[7], NULL) : 0.0;
		if (f[8][0])
			fix->course_deg = strtod(f[8], NULL);
		parse_date(f[9], fix);
		return RTD_NMEA_RMC;
	}

	if (strcmp(type, "GGA") == 0 && n >= 10) {
		fix->quality = atoi(f[6]);
		fix->sats = atoi(f[7]);
		fix->hdop = f[8][0] ? strtod(f[8], NULL) : 0.0;
		fix->alt_m = f[9][0] ? strtod(f[9], NULL) : 0.0;
		return RTD_NMEA_GGA;
	}

	return RTD_NMEA_OTHER;
}

int64_t rtd_fix_utc_us(const rtd_fix_t *fix)
{
	if (!fix->have_time || !fix->have_date || fix->year < 2000)
		return 0;
	struct tm tm = {
		.tm_year = fix->year - 1900,
		.tm_mon = fix->mon - 1,
		.tm_mday = fix->day,
		.tm_hour = fix->hour,
		.tm_min = fix->min,
		.tm_sec = (int)fix->sec,
	};
	time_t t = timegm(&tm);
	if (t == (time_t)-1)
		return 0;
	return (int64_t)t * 1000000 + (int64_t)((fix->sec - (int)fix->sec) * 1e6);
}

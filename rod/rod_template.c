/*
 * rod_template.c -- Template variable expansion and IP resolution
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "rod.h"

static void resolve_default_iface(char *out, int out_size)
{
	FILE *f = fopen("/proc/net/route", "r");
	if (!f) {
		out[0] = '\0';
		return;
	}
	char line[256];
	out[0] = '\0';
	while (fgets(line, sizeof(line), f)) {
		char iface[16];
		unsigned dest;
		if (sscanf(line, "%15s %x", iface, &dest) == 2 && dest == 0) {
			rss_strlcpy(out, iface, out_size);
			break;
		}
	}
	fclose(f);
}

static void refresh_ip_addrs(rod_state_t *st)
{
	int64_t now = rss_timestamp_us();
	if (st->ip_refresh_ts && now - st->ip_refresh_ts < 60000000)
		return;
	st->ip_refresh_ts = now;

	char iface[16] = "";
	resolve_default_iface(iface, sizeof(iface));

	st->ip[0] = '\0';
	st->ip6[0] = '\0';

	struct ifaddrs *ifa_list, *ifa;
	if (getifaddrs(&ifa_list) < 0)
		return;

	for (ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
		if (!ifa->ifa_addr || (ifa->ifa_flags & IFF_LOOPBACK))
			continue;

		bool match = iface[0] ? strcmp(ifa->ifa_name, iface) == 0 : true;
		if (!match)
			continue;

		if (ifa->ifa_addr->sa_family == AF_INET && !st->ip[0]) {
			struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
			inet_ntop(AF_INET, &sa->sin_addr, st->ip, sizeof(st->ip));
		} else if (ifa->ifa_addr->sa_family == AF_INET6 && !st->ip6[0]) {
			struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)ifa->ifa_addr;
			if (!IN6_IS_ADDR_LINKLOCAL(&sa6->sin6_addr)) {
				inet_ntop(AF_INET6, &sa6->sin6_addr, st->ip6, sizeof(st->ip6));
			}
		}

		if (st->ip[0] && st->ip6[0])
			break;
	}
	freeifaddrs(ifa_list);
}

static void format_uptime(char *buf, size_t bufsz)
{
	FILE *f = fopen("/proc/uptime", "r");
	if (!f) {
		snprintf(buf, bufsz, "Up: ?");
		return;
	}
	double up = 0;
	if (fscanf(f, "%lf", &up) != 1)
		up = 0;
	fclose(f);

	int sec = (int)up;
	int days = sec / 86400;
	int hours = (sec % 86400) / 3600;
	int mins = (sec % 3600) / 60;
	int secs = sec % 60;

	if (days > 0)
		snprintf(buf, bufsz, "%dd %dh %dm %ds", days, hours, mins, secs);
	else if (hours > 0)
		snprintf(buf, bufsz, "%dh %dm %ds", hours, mins, secs);
	else
		snprintf(buf, bufsz, "%dm %ds", mins, secs);
}

int rod_expand_template(rod_state_t *st, const char *tmpl, char *out, int out_size)
{
	int pos = 0;
	const char *p = tmpl;

	while (*p && pos < out_size - 1) {
		if (*p != '%') {
			out[pos++] = *p++;
			continue;
		}

		const char *end = strchr(p + 1, '%');
		if (!end) {
			out[pos++] = *p++;
			continue;
		}

		int vlen = (int)(end - p - 1);
		if (vlen <= 0 || vlen > 31) {
			out[pos++] = *p++;
			continue;
		}

		char varname[32];
		memcpy(varname, p + 1, vlen);
		varname[vlen] = '\0';

		char val[128] = "";
		if (strcmp(varname, "time") == 0) {
			time_t now = time(NULL);
			struct tm tm;
			localtime_r(&now, &tm);
			strftime(val, sizeof(val), st->settings.time_format, &tm);
		} else if (strcmp(varname, "uptime") == 0) {
			format_uptime(val, sizeof(val));
		} else if (strcmp(varname, "hostname") == 0) {
			rss_strlcpy(val, st->hostname, sizeof(val));
		} else if (strcmp(varname, "ip") == 0) {
			refresh_ip_addrs(st);
			rss_strlcpy(val, st->ip, sizeof(val));
		} else if (strcmp(varname, "ip6") == 0) {
			refresh_ip_addrs(st);
			rss_strlcpy(val, st->ip6, sizeof(val));
		} else {
			for (int i = 0; i < st->var_count; i++) {
				if (strcmp(st->vars[i].name, varname) == 0) {
					rss_strlcpy(val, st->vars[i].value, sizeof(val));
					break;
				}
			}
		}

		int vallen = (int)strlen(val);
		if (pos + vallen < out_size) {
			memcpy(out + pos, val, vallen);
			pos += vallen;
		}
		p = end + 1;
	}

	out[pos] = '\0';
	return pos;
}

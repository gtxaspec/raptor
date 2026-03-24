/*
 * ric_main.c -- Raptor IR-Cut Day/Night Control Daemon
 *
 * Monitors ISP exposure via HAL and switches between day and night
 * modes with hysteresis. Controls IR-cut filter (single or dual GPIO)
 * and IR LED enable.
 *
 * Supports manual override via raptorctl: ric mode auto|day|night
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <fcntl.h>
#include <sys/epoll.h>

#include "ric.h"

static void load_config(ric_state_t *st)
{
	rss_config_t *cfg = st->config;
	ric_config_t *c = &st->cfg;

	c->enabled = rss_config_get_bool(cfg, "ircut", "enabled", true);

	const char *mode = rss_config_get_str(cfg, "ircut", "mode", "auto");
	if (strcmp(mode, "day") == 0)
		c->opmode = RIC_FORCE_DAY;
	else if (strcmp(mode, "night") == 0)
		c->opmode = RIC_FORCE_NIGHT;
	else
		c->opmode = RIC_AUTO;

	c->gpio_ircut = rss_config_get_int(cfg, "ircut", "gpio_ircut", -1);
	c->gpio_ircut2 = rss_config_get_int(cfg, "ircut", "gpio_ircut2", -1);
	c->gpio_irled = rss_config_get_int(cfg, "ircut", "gpio_irled", -1);

	c->night_threshold = rss_config_get_int(cfg, "ircut", "night_threshold", 40000);
	c->day_threshold = rss_config_get_int(cfg, "ircut", "day_threshold", 25000);
	c->hysteresis_sec = rss_config_get_int(cfg, "ircut", "hysteresis_sec", 5);
	c->poll_interval_ms = rss_config_get_int(cfg, "ircut", "poll_interval_ms", 1000);
}

/* ── Control socket ── */

static int json_get_str(const char *json, const char *key, char *buf, int bufsz)
{
	char pattern[64];
	snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
	const char *p = strstr(json, pattern);
	if (!p)
		return -1;
	p += strlen(pattern);
	const char *end = strchr(p, '"');
	if (!end)
		return -1;
	int len = (int)(end - p);
	if (len >= bufsz)
		len = bufsz - 1;
	memcpy(buf, p, len);
	buf[len] = '\0';
	return 0;
}

static int ric_ctrl_handler(const char *cmd_json, char *resp_buf, int resp_buf_size, void *userdata)
{
	ric_state_t *st = userdata;

	if (strstr(cmd_json, "\"mode\"")) {
		char val[16];
		if (json_get_str(cmd_json, "value", val, sizeof(val)) == 0) {
			if (strcmp(val, "day") == 0) {
				st->cfg.opmode = RIC_FORCE_DAY;
				ric_set_mode(st, RIC_MODE_DAY);
			} else if (strcmp(val, "night") == 0) {
				st->cfg.opmode = RIC_FORCE_NIGHT;
				ric_set_mode(st, RIC_MODE_NIGHT);
			} else {
				st->cfg.opmode = RIC_AUTO;
			}
			rss_config_set_str(st->config, "ircut", "mode", val);
		}
		snprintf(resp_buf, resp_buf_size,
			 "{\"status\":\"ok\",\"mode\":\"%s\",\"state\":\"%s\"}",
			 st->cfg.opmode == RIC_AUTO
				 ? "auto"
				 : (st->cfg.opmode == RIC_FORCE_DAY ? "day" : "night"),
			 st->current_mode == RIC_MODE_DAY ? "day" : "night");
		return (int)strlen(resp_buf);
	}

	if (strstr(cmd_json, "\"config-get\"")) {
		char section[64], key[64];
		if (json_get_str(cmd_json, "section", section, sizeof(section)) == 0 &&
		    json_get_str(cmd_json, "key", key, sizeof(key)) == 0) {
			const char *v = rss_config_get_str(st->config, section, key, NULL);
			if (v)
				snprintf(resp_buf, resp_buf_size, "%s", v);
			else
				resp_buf[0] = '\0';
		} else {
			resp_buf[0] = '\0';
		}
		return (int)strlen(resp_buf);
	}

	if (strstr(cmd_json, "\"config-save\"")) {
		int ret = rss_config_save(st->config, st->config_path);
		snprintf(resp_buf, resp_buf_size, "{\"status\":\"%s\"}", ret == 0 ? "ok" : "error");
		if (ret == 0)
			RSS_INFO("running config saved to %s", st->config_path);
		return (int)strlen(resp_buf);
	}

	if (strstr(cmd_json, "\"config-show\"")) {
		IMPISPEVAttr ev = {0};
		IMP_ISP_Tuning_GetEVAttr(&ev);
		snprintf(resp_buf, resp_buf_size,
			 "{\"status\":\"ok\",\"mode\":\"%s\",\"state\":\"%s\","
			 "\"again\":%u,\"dgain\":%u,\"ev\":%u,"
			 "\"night_threshold\":%d,\"day_threshold\":%d}",
			 st->cfg.opmode == RIC_AUTO
				 ? "auto"
				 : (st->cfg.opmode == RIC_FORCE_DAY ? "day" : "night"),
			 st->current_mode == RIC_MODE_DAY ? "day" : "night", ev.again, ev.dgain,
			 ev.ev, st->cfg.night_threshold, st->cfg.day_threshold);
		return (int)strlen(resp_buf);
	}

	/* Default: status */
	IMPISPEVAttr ev = {0};
	IMP_ISP_Tuning_GetEVAttr(&ev);
	snprintf(resp_buf, resp_buf_size,
		 "{\"status\":\"ok\",\"mode\":\"%s\",\"state\":\"%s\","
		 "\"again\":%u,\"dgain\":%u}",
		 st->cfg.opmode == RIC_AUTO ? "auto"
					    : (st->cfg.opmode == RIC_FORCE_DAY ? "day" : "night"),
		 st->current_mode == RIC_MODE_DAY ? "day" : "night", ev.again, ev.dgain);
	return (int)strlen(resp_buf);
}

/* ── Entry point ── */

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"  -c <file>   Config file (default: /etc/raptor.conf)\n"
		"  -f          Run in foreground\n"
		"  -d          Debug logging\n"
		"  -h          Show this help\n",
		prog);
}

int main(int argc, char **argv)
{
	const char *config_path = "/etc/raptor.conf";
	bool foreground = false;
	bool debug = false;
	int opt;

	while ((opt = getopt(argc, argv, "c:fdh")) != -1) {
		switch (opt) {
		case 'c':
			config_path = optarg;
			break;
		case 'f':
			foreground = true;
			break;
		case 'd':
			debug = true;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	rss_log_init("ric", debug ? RSS_LOG_DEBUG : RSS_LOG_INFO,
		     foreground ? RSS_LOG_TARGET_STDERR : RSS_LOG_TARGET_SYSLOG, NULL);

	rss_config_t *cfg = rss_config_load(config_path);
	if (!cfg) {
		RSS_FATAL("failed to load config: %s", config_path);
		return 1;
	}

	if (!rss_config_get_bool(cfg, "ircut", "enabled", true)) {
		RSS_INFO("IR-cut control disabled in config, exiting");
		rss_config_free(cfg);
		return 0;
	}

	if (!foreground) {
		if (rss_daemonize("ric", false) < 0) {
			RSS_FATAL("daemonize failed");
			rss_config_free(cfg);
			return 1;
		}
	}

	volatile sig_atomic_t *running = rss_signal_init();
	RSS_INFO("ric starting");

	ric_state_t st = {0};
	st.config = cfg;
	st.config_path = config_path;
	st.running = running;
	st.current_mode = RIC_MODE_DAY;
	load_config(&st);

	/* Wait for RVD to initialize ISP before querying exposure.
	 * RIC uses libimp tuning functions directly (no HAL init needed —
	 * libimp.so is shared, ISP already initialized by RVD). */
	RSS_INFO("waiting for ISP...");
	for (int i = 0; i < 100 && *running; i++) {
		IMPISPEVAttr ev;
		if (IMP_ISP_Tuning_GetEVAttr(&ev) == 0) {
			RSS_INFO("ISP ready (again=%u dgain=%u)", ev.again, ev.dgain);
			break;
		}
		usleep(100000);
	}

	/* Init GPIOs */
	ric_gpio_init(&st);

	/* Apply initial mode */
	if (st.cfg.opmode == RIC_FORCE_DAY)
		ric_set_mode(&st, RIC_MODE_DAY);
	else if (st.cfg.opmode == RIC_FORCE_NIGHT)
		ric_set_mode(&st, RIC_MODE_NIGHT);
	else
		ric_set_mode(&st, RIC_MODE_DAY); /* start in day, auto will adjust */

	/* Control socket */
	rss_mkdir_p("/var/run/rss");
	st.ctrl = rss_ctrl_listen("/var/run/rss/ric.sock");

	int epoll_fd = -1;
	int ctrl_fd = st.ctrl ? rss_ctrl_get_fd(st.ctrl) : -1;
	if (ctrl_fd >= 0) {
		epoll_fd = epoll_create1(0);
		if (epoll_fd >= 0) {
			struct epoll_event ev = {.events = EPOLLIN, .data.fd = ctrl_fd};
			epoll_ctl(epoll_fd, EPOLL_CTL_ADD, ctrl_fd, &ev);
		}
	}

	RSS_INFO("ric running (mode=%s, gpio_ircut=%d, gpio_ircut2=%d, gpio_irled=%d)",
		 st.cfg.opmode == RIC_AUTO ? "auto"
					   : (st.cfg.opmode == RIC_FORCE_DAY ? "day" : "night"),
		 st.cfg.gpio_ircut, st.cfg.gpio_ircut2, st.cfg.gpio_irled);

	/* Main loop: poll exposure + handle control socket */
	while (*running) {
		ric_poll_exposure(&st);

		if (epoll_fd >= 0) {
			struct epoll_event ev;
			int n = epoll_wait(epoll_fd, &ev, 1, st.cfg.poll_interval_ms);
			if (n > 0 && st.ctrl)
				rss_ctrl_accept_and_handle(st.ctrl, ric_ctrl_handler, &st);
		} else {
			usleep(st.cfg.poll_interval_ms * 1000);
		}
	}

	RSS_INFO("ric shutting down");

	if (epoll_fd >= 0)
		close(epoll_fd);
	if (st.ctrl)
		rss_ctrl_destroy(st.ctrl);

cleanup:
	rss_config_free(cfg);
	if (!foreground)
		rss_daemon_cleanup("ric");

	return 0;
}

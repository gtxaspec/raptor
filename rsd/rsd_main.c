/*
 * rsd_main.c -- Raptor RTSP Streaming Daemon entry point
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>

#include "rsd.h"

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
		case 'c': config_path = optarg; break;
		case 'f': foreground = true; break;
		case 'd': debug = true; break;
		case 'h': usage(argv[0]); return 0;
		default:  usage(argv[0]); return 1;
		}
	}

	/* Seed PRNG for SSRC generation (compy requirement) */
	srand(time(NULL));

	rss_log_init("rsd",
		     debug ? RSS_LOG_DEBUG : RSS_LOG_INFO,
		     foreground ? RSS_LOG_TARGET_STDERR : RSS_LOG_TARGET_SYSLOG,
		     NULL);

	rss_config_t *cfg = rss_config_load(config_path);
	if (!cfg) {
		RSS_FATAL("failed to load config: %s", config_path);
		return 1;
	}

	if (!foreground) {
		if (rss_daemonize("rsd", false) < 0) {
			RSS_FATAL("daemonize failed");
			rss_config_free(cfg);
			return 1;
		}
	}

	volatile sig_atomic_t *running = rss_signal_init();

	RSS_INFO("rsd starting");

	rsd_server_t srv = {0};
	srv.cfg = cfg;
	srv.config_path = config_path;
	srv.running = running;
	srv.port = rss_config_get_int(cfg, "rtsp", "port", 554);
	pthread_mutex_init(&srv.clients_lock, NULL);

	int ret = rsd_server_init(&srv);
	if (ret != 0) {
		RSS_FATAL("server init failed");
		goto cleanup;
	}

	rsd_server_run(&srv);

	RSS_INFO("rsd shutting down");

cleanup:
	rsd_server_deinit(&srv);
	pthread_mutex_destroy(&srv.clients_lock);
	rss_config_free(cfg);

	if (!foreground)
		rss_daemon_cleanup("rsd");

	return 0;
}

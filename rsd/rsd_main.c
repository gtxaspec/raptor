/*
 * rsd_main.c -- Raptor RTSP Streaming Daemon entry point
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>

#include "rsd.h"

/* ── Digest auth credential lookup ── */

typedef struct {
	char username[128];
	char password[128];
} rsd_credentials_t;

static bool rsd_credential_lookup(const char *username, char *password_out, size_t password_max,
				  void *user_data)
{
	const rsd_credentials_t *creds = user_data;
	if (strcmp(username, creds->username) != 0)
		return false;
	strncpy(password_out, creds->password, password_max - 1);
	password_out[password_max - 1] = '\0';
	return true;
}

/* Static credentials — lives for the process lifetime */
static rsd_credentials_t g_creds;

static Compy_Auth *rsd_auth_new(const char *username, const char *password)
{
	strncpy(g_creds.username, username, sizeof(g_creds.username) - 1);
	strncpy(g_creds.password, password, sizeof(g_creds.password) - 1);
	return Compy_Auth_new("Raptor", rsd_credential_lookup, &g_creds);
}

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

	/* Seed PRNG for SSRC generation (compy requirement) */
	srand(time(NULL));

	rss_log_init("rsd", debug ? RSS_LOG_DEBUG : RSS_LOG_INFO,
		     foreground ? RSS_LOG_TARGET_STDERR : RSS_LOG_TARGET_SYSLOG, NULL);

	rss_config_t *cfg = rss_config_load(config_path);
	if (!cfg) {
		RSS_FATAL("failed to load config: %s", config_path);
		return 1;
	}

	if (rss_daemonize("rsd", foreground) < 0) {
		RSS_FATAL("daemonize failed");
		rss_config_free(cfg);
		return 1;
	}

	volatile sig_atomic_t *running = rss_signal_init();

	RSS_INFO("rsd starting");

	rsd_server_t srv = {0};
	srv.cfg = cfg;
	srv.config_path = config_path;
	srv.running = running;
	srv.port = rss_config_get_int(cfg, "rtsp", "port", 554);
	pthread_mutex_init(&srv.clients_lock, NULL);

	/* Digest auth — enabled when both username and password are set */
	const char *rtsp_user = rss_config_get_str(cfg, "rtsp", "username", "");
	const char *rtsp_pass = rss_config_get_str(cfg, "rtsp", "password", "");
	if (rtsp_user[0] && rtsp_pass[0]) {
		srv.auth = rsd_auth_new(rtsp_user, rtsp_pass);
		if (srv.auth)
			RSS_INFO("RTSP Digest auth enabled (realm=Raptor)");
		else
			RSS_WARN("failed to create auth context");
	}

	int ret = rsd_server_init(&srv);
	if (ret != 0) {
		RSS_FATAL("server init failed");
		goto cleanup;
	}

	rsd_server_run(&srv);

	RSS_INFO("rsd shutting down");

cleanup:
	if (srv.auth)
		Compy_Auth_free(srv.auth);
	rsd_server_deinit(&srv);
	pthread_mutex_destroy(&srv.clients_lock);
	rss_config_free(cfg);

	rss_daemon_cleanup("rsd");

	return 0;
}

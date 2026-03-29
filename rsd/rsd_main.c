/*
 * rsd_main.c -- Raptor RTSP Streaming Daemon entry point
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

int main(int argc, char **argv)
{
	/* Seed PRNG for SSRC generation (compy requirement) */
	srand(time(NULL));

	rss_daemon_ctx_t dctx;
	int ret = rss_daemon_init(&dctx, "rsd", argc, argv);
	if (ret != 0)
		return ret < 0 ? 1 : 0;

	if (!rss_config_get_bool(dctx.cfg, "rtsp", "enabled", true)) {
		RSS_INFO("RTSP disabled in config");
		rss_config_free(dctx.cfg);
		rss_daemon_cleanup("rsd");
		return 0;
	}

	rsd_server_t srv = {0};
	srv.cfg = dctx.cfg;
	srv.config_path = dctx.config_path;
	srv.running = dctx.running;
	srv.port = rss_config_get_int(dctx.cfg, "rtsp", "port", 554);
	srv.max_clients = rss_config_get_int(dctx.cfg, "rtsp", "max_clients", RSD_MAX_CLIENTS);
	if (srv.max_clients < 1) srv.max_clients = 1;
	if (srv.max_clients > RSD_MAX_CLIENTS) srv.max_clients = RSD_MAX_CLIENTS;
	rss_strlcpy(srv.endpoint_main, rss_config_get_str(dctx.cfg, "rtsp", "endpoint_main", ""),
		    sizeof(srv.endpoint_main));
	rss_strlcpy(srv.endpoint_sub, rss_config_get_str(dctx.cfg, "rtsp", "endpoint_sub", ""),
		    sizeof(srv.endpoint_sub));
	pthread_mutex_init(&srv.clients_lock, NULL);

	/* Digest auth — enabled when both username and password are set */
	const char *rtsp_user = rss_config_get_str(dctx.cfg, "rtsp", "username", "");
	const char *rtsp_pass = rss_config_get_str(dctx.cfg, "rtsp", "password", "");
	if (rtsp_user[0] && rtsp_pass[0]) {
		srv.auth = rsd_auth_new(rtsp_user, rtsp_pass);
		if (srv.auth)
			RSS_INFO("RTSP Digest auth enabled (realm=Raptor)");
		else
			RSS_WARN("failed to create auth context");
	}

#ifdef COMPY_HAS_TLS
	/* RTSPS — enabled when tls = true and cert/key paths are set */
	const char *tls_cert = rss_config_get_str(dctx.cfg, "rtsp", "tls_cert", "");
	const char *tls_key = rss_config_get_str(dctx.cfg, "rtsp", "tls_key", "");
	if (rss_config_get_bool(dctx.cfg, "rtsp", "tls", false) && tls_cert[0] && tls_key[0]) {
		Compy_TlsConfig tls_cfg = {.cert_path = tls_cert, .key_path = tls_key};
		srv.tls_ctx = Compy_TlsContext_new(tls_cfg);
		if (srv.tls_ctx) {
			/* RTSPS: use tls_port if set, otherwise fall back to port */
			srv.port = rss_config_get_int(dctx.cfg, "rtsp", "tls_port", srv.port);
			RSS_INFO("RTSPS enabled (cert=%s, port=%d)", tls_cert, srv.port);
		} else {
			RSS_FATAL("TLS enabled but failed to load cert/key (cert=%s, key=%s)",
				  tls_cert, tls_key);
			rss_config_free(dctx.cfg);
			rss_daemon_cleanup("rsd");
			return 1;
		}
	}
#endif

	ret = rsd_server_init(&srv);
	if (ret != 0) {
		RSS_FATAL("server init failed");
		goto cleanup;
	}

	rsd_server_run(&srv);

	RSS_INFO("rsd shutting down");

cleanup:
#ifdef COMPY_HAS_TLS
	if (srv.tls_ctx)
		Compy_TlsContext_free(srv.tls_ctx);
#endif
	if (srv.auth)
		Compy_Auth_free(srv.auth);
	rsd_server_deinit(&srv);
	pthread_mutex_destroy(&srv.clients_lock);
	rss_config_free(dctx.cfg);

	rss_daemon_cleanup("rsd");

	return 0;
}

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
	rss_strlcpy(password_out, creds->password, password_max);
	return true;
}

/* Static credentials — lives for the process lifetime */
static rsd_credentials_t g_creds;

static Compy_Auth *rsd_auth_new(const char *username, const char *password)
{
	rss_strlcpy(g_creds.username, username, sizeof(g_creds.username));
	rss_strlcpy(g_creds.password, password, sizeof(g_creds.password));
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
	bool tls_enabled = false;
#ifdef COMPY_HAS_TLS
	tls_enabled = rss_config_get_bool(dctx.cfg, "rtsp", "tls", false);
#endif
	srv.port = rss_config_get_int(dctx.cfg, "rtsp", "port", tls_enabled ? 322 : 554);
	srv.max_clients = rss_config_get_int(dctx.cfg, "rtsp", "max_clients", RSD_MAX_CLIENTS);
	if (srv.max_clients < 1)
		srv.max_clients = 1;
	if (srv.max_clients > RSD_MAX_CLIENTS)
		srv.max_clients = RSD_MAX_CLIENTS;
	srv.session_timeout = rss_config_get_int(dctx.cfg, "rtsp", "session_timeout", 60);
	if (srv.session_timeout < 10)
		srv.session_timeout = 10;
	srv.tcp_sndbuf = rss_config_get_int(dctx.cfg, "rtsp", "tcp_sndbuf", 64 * 1024);
	rss_strlcpy(srv.endpoint_main, rss_config_get_str(dctx.cfg, "rtsp", "endpoint_main", ""),
		    sizeof(srv.endpoint_main));
	rss_strlcpy(srv.endpoint_sub, rss_config_get_str(dctx.cfg, "rtsp", "endpoint_sub", ""),
		    sizeof(srv.endpoint_sub));
	rss_strlcpy(srv.session_name,
		    rss_config_get_str(dctx.cfg, "rtsp", "session_name", "Raptor Live"),
		    sizeof(srv.session_name));
	rss_strlcpy(srv.session_info, rss_config_get_str(dctx.cfg, "rtsp", "session_info", ""),
		    sizeof(srv.session_info));
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
	/* RTSPS — enabled when tls = true in config */
	if (tls_enabled) {
		const char *tls_cert = rss_config_get_str(dctx.cfg, "rtsp", "tls_cert",
							  "/etc/ssl/certs/uhttpd.crt");
		const char *tls_key = rss_config_get_str(dctx.cfg, "rtsp", "tls_key",
							 "/etc/ssl/private/uhttpd.key");
		Compy_TlsConfig tls_cfg = {.cert_path = tls_cert, .key_path = tls_key};
		srv.tls_ctx = Compy_TlsContext_new(tls_cfg);
		if (srv.tls_ctx) {
			RSS_INFO("RTSPS enabled (cert=%s, port=%d)", tls_cert, srv.port);
		} else {
			RSS_WARN("TLS enabled but cert/key not usable (cert=%s, key=%s), "
				 "falling back to plain RTSP on port 554",
				 tls_cert, tls_key);
			srv.port = 554;
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

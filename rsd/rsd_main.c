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
	/* Seed PRNG for SSRC generation (compy requirement).
	 * Use /dev/urandom — time(NULL) is predictable (RFC 3550 §8.1). */
	{
		unsigned int seed;
		FILE *f = fopen("/dev/urandom", "r");
		if (f && fread(&seed, sizeof(seed), 1, f) == 1)
			srand(seed);
		else
			srand((unsigned)time(NULL) ^ (unsigned)getpid());
		if (f)
			fclose(f);
	}

	rss_daemon_ctx_t dctx;
	int ret = rss_daemon_init(&dctx, "rsd", argc, argv,
				  ""
#ifdef COMPY_HAS_TLS
				  " tls"
#endif
	);
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
	srv.rtcp_sr = rss_config_get_bool(dctx.cfg, "rtsp", "rtcp_sr", false);
	rsd_endpoints_load(&srv, dctx.cfg);
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
	} else if (rtsp_user[0] || rtsp_pass[0]) {
		RSS_WARN("RTSP auth requires both username and password — auth disabled");
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
			/*
			 * Optional ciphersuite preference. Useful on slow SoCs
			 * where the per-record cost of AES-GCM via a kernel
			 * crypto engine outweighs software ChaCha20 — see
			 * Compy_TlsCipherPreference docs in <compy/tls.h>.
			 *
			 * Values: "default" (backend default, typically GCM),
			 *         "chacha20" (force TLS 1.3 CHACHA20-POLY1305).
			 */
			const char *pref = rss_config_get_str(dctx.cfg, "rtsp",
							      "tls_cipher_preference", "default");
			Compy_TlsCipherPreference pref_val = COMPY_TLS_CIPHER_DEFAULT;
			if (strcmp(pref, "chacha20") == 0) {
				pref_val = COMPY_TLS_CIPHER_CHACHA20_ONLY;
			} else if (strcmp(pref, "default") != 0) {
				RSS_WARN("unknown tls_cipher_preference '%s', "
					 "using backend default",
					 pref);
			}
			if (pref_val != COMPY_TLS_CIPHER_DEFAULT) {
				if (Compy_TlsContext_set_cipher_preference(srv.tls_ctx, pref_val) !=
				    0) {
					RSS_WARN("TLS backend does not support "
						 "cipher preference");
				}
			}
			RSS_INFO("RTSPS enabled (cert=%s, port=%d, ciphers=%s)", tls_cert, srv.port,
				 pref);
		} else {
			RSS_WARN("TLS enabled but cert/key not usable (cert=%s, key=%s), "
				 "falling back to plain RTSP on port 554",
				 tls_cert, tls_key);
			srv.port = 554;
		}
	}
#endif

	RSS_INFO("RTSP listening on port %d%s", srv.port, tls_enabled ? " (TLS)" : "");
	RSS_DEBUG("  max_clients=%d session_timeout=%d tcp_sndbuf=%d rtcp_sr=%d", srv.max_clients,
		  srv.session_timeout, srv.tcp_sndbuf, srv.rtcp_sr);

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

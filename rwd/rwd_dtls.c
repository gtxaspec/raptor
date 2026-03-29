/*
 * rwd_dtls.c -- DTLS-SRTP wrapper (mbedTLS)
 *
 * Manages the DTLS server context and per-client handshakes.
 * After handshake, exports SRTP keying material for compy's
 * SRTP transport layer.
 *
 * Requires mbedTLS compiled with MBEDTLS_SSL_DTLS_SRTP.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>

#include "rwd.h"

#include <mbedtls/error.h>
#include <mbedtls/md.h>

#ifndef MBEDTLS_SSL_DTLS_SRTP
#error "RWD requires mbedTLS compiled with MBEDTLS_SSL_DTLS_SRTP support"
#endif

/* SRTP keying material sizes for AES_128_CM_HMAC_SHA1_80 */
#define SRTP_MASTER_KEY_LEN   16
#define SRTP_MASTER_SALT_LEN  14
#define SRTP_KEY_MATERIAL_LEN (2 * SRTP_MASTER_KEY_LEN + 2 * SRTP_MASTER_SALT_LEN)

/* ── BIO callbacks for non-blocking UDP I/O ── */

static int rwd_dtls_bio_send(void *ctx, const unsigned char *buf, size_t len)
{
	rwd_client_t *c = ctx;
	ssize_t ret = sendto(c->server->udp_fd, buf, len, 0, (const struct sockaddr *)&c->addr,
			     c->addr_len);
	if (ret < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return MBEDTLS_ERR_SSL_WANT_WRITE;
		return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
	}
	return (int)ret;
}

static int rwd_dtls_bio_recv(void *ctx, unsigned char *buf, size_t len)
{
	rwd_client_t *c = ctx;

	if (c->dtls_buf_len == 0)
		return MBEDTLS_ERR_SSL_WANT_READ;

	size_t copy = c->dtls_buf_len < len ? c->dtls_buf_len : len;
	memcpy(buf, c->dtls_buf, copy);
	c->dtls_buf_len = 0;
	return (int)copy;
}

static int rwd_dtls_bio_recv_timeout(void *ctx, unsigned char *buf, size_t len, uint32_t timeout)
{
	(void)timeout;
	return rwd_dtls_bio_recv(ctx, buf, len);
}

/* ── Compute SHA-256 fingerprint of certificate ── */

static int compute_cert_fingerprint(const mbedtls_x509_crt *cert, char *buf, size_t buf_size)
{
	uint8_t hash[32];

	int ret = mbedtls_sha256(cert->raw.p, cert->raw.len, hash, 0);
	if (ret != 0)
		return -1;

	/* Format: "sha-256 AA:BB:CC:DD:..." */
	int off = snprintf(buf, buf_size, "sha-256 ");
	for (int i = 0; i < 32 && (size_t)off < buf_size - 3; i++) {
		if (i > 0)
			buf[off++] = ':';
		off += snprintf(buf + off, buf_size - off, "%02X", hash[i]);
	}
	return 0;
}

/* ── Initialize shared DTLS server context ──
 *
 * On early return (failure), the caller is responsible for calling
 * rwd_dtls_free() which safely frees all partially-initialized
 * mbedTLS contexts (mbedtls_*_free functions are no-ops on zeroed state). */

int rwd_dtls_init(rwd_dtls_ctx_t *ctx, const char *cert_path, const char *key_path)
{
	int ret;
	char errbuf[128];

	memset(ctx, 0, sizeof(*ctx));

	mbedtls_ssl_config_init(&ctx->conf);
	mbedtls_x509_crt_init(&ctx->cert);
	mbedtls_pk_init(&ctx->key);
	mbedtls_entropy_init(&ctx->entropy);
	mbedtls_ctr_drbg_init(&ctx->ctr_drbg);
	mbedtls_ssl_cookie_init(&ctx->cookie);

	/* Seed CSPRNG */
	ret = mbedtls_ctr_drbg_seed(&ctx->ctr_drbg, mbedtls_entropy_func, &ctx->entropy,
				    (const unsigned char *)"rwd", 3);
	if (ret != 0) {
		mbedtls_strerror(ret, errbuf, sizeof(errbuf));
		RSS_ERROR("DTLS: ctr_drbg_seed failed: %s", errbuf);
		return -1;
	}

	/* Load certificate */
	ret = mbedtls_x509_crt_parse_file(&ctx->cert, cert_path);
	if (ret != 0) {
		mbedtls_strerror(ret, errbuf, sizeof(errbuf));
		RSS_ERROR("DTLS: failed to load cert %s: %s", cert_path, errbuf);
		return -1;
	}

	/* Load private key */
	ret = mbedtls_pk_parse_keyfile(&ctx->key, key_path, NULL, mbedtls_ctr_drbg_random,
				       &ctx->ctr_drbg);
	if (ret != 0) {
		mbedtls_strerror(ret, errbuf, sizeof(errbuf));
		RSS_ERROR("DTLS: failed to load key %s: %s", key_path, errbuf);
		return -1;
	}

	/* Configure SSL as DTLS server */
	ret = mbedtls_ssl_config_defaults(&ctx->conf, MBEDTLS_SSL_IS_SERVER,
					  MBEDTLS_SSL_TRANSPORT_DATAGRAM,
					  MBEDTLS_SSL_PRESET_DEFAULT);
	if (ret != 0) {
		mbedtls_strerror(ret, errbuf, sizeof(errbuf));
		RSS_ERROR("DTLS: ssl_config_defaults failed: %s", errbuf);
		return -1;
	}

	mbedtls_ssl_conf_rng(&ctx->conf, mbedtls_ctr_drbg_random, &ctx->ctr_drbg);
	mbedtls_ssl_conf_ca_chain(&ctx->conf, &ctx->cert, NULL);

	ret = mbedtls_ssl_conf_own_cert(&ctx->conf, &ctx->cert, &ctx->key);
	if (ret != 0) {
		mbedtls_strerror(ret, errbuf, sizeof(errbuf));
		RSS_ERROR("DTLS: ssl_conf_own_cert failed: %s", errbuf);
		return -1;
	}

	/* WebRTC uses self-signed certs — trust is via SDP fingerprint */
	mbedtls_ssl_conf_authmode(&ctx->conf, MBEDTLS_SSL_VERIFY_NONE);

	/* Prefer SHA256-based ciphersuites for SRTP key derivation compatibility.
	 * Include SHA1 fallbacks for broader client support (Chrome, go2rtc).
	 * SHA384 ciphersuites are excluded — the TLS PRF fallback for SRTP key
	 * export produces wrong keys with SHA384 (go2rtc/pion). */
	/* static: mbedtls_ssl_conf_ciphersuites stores a pointer, not a copy */
	static const int ciphersuites[] = {
		MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
		MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
		MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256,
		MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256,
		MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA,
		MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA,
		MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
		MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
		0,
	};
	mbedtls_ssl_conf_ciphersuites(&ctx->conf, ciphersuites);

	/* DTLS-SRTP protection profiles (UNSET-terminated) */
	static const mbedtls_ssl_srtp_profile profiles[] = {
		MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80,
		MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_32,
		MBEDTLS_TLS_SRTP_UNSET,
	};
	mbedtls_ssl_conf_dtls_srtp_protection_profiles(&ctx->conf, profiles);

	/* Disable DTLS cookies (HelloVerifyRequest) — ICE already verifies
	 * the client's address, and cookies cause issues with some browsers */
	mbedtls_ssl_conf_dtls_cookies(&ctx->conf, NULL, NULL, NULL);

	/* Compute certificate fingerprint for SDP */
	if (compute_cert_fingerprint(&ctx->cert, ctx->fingerprint, sizeof(ctx->fingerprint)) != 0) {
		RSS_ERROR("DTLS: failed to compute cert fingerprint");
		return -1;
	}

	RSS_INFO("DTLS: initialized (%s)", ctx->fingerprint);
	return 0;
}

void rwd_dtls_free(rwd_dtls_ctx_t *ctx)
{
	mbedtls_ssl_cookie_free(&ctx->cookie);
	mbedtls_ctr_drbg_free(&ctx->ctr_drbg);
	mbedtls_entropy_free(&ctx->entropy);
	mbedtls_pk_free(&ctx->key);
	mbedtls_x509_crt_free(&ctx->cert);
	mbedtls_ssl_config_free(&ctx->conf);
}

/* ── Key export callback — captures master secret + randoms ── */

static void rwd_dtls_key_export_cb(void *p_expkey, mbedtls_ssl_key_export_type type,
				   const unsigned char *secret, size_t secret_len,
				   const unsigned char client_random[32],
				   const unsigned char server_random[32],
				   mbedtls_tls_prf_types tls_prf_type)
{
	rwd_client_t *c = p_expkey;
	(void)type;

	if (secret_len > 48)
		secret_len = 48;
	memcpy(c->master_secret, secret, secret_len);
	/* RFC 5705: seed = client_random + server_random */
	memcpy(c->randbytes, client_random, 32);
	memcpy(c->randbytes + 32, server_random, 32);
	c->tls_prf = tls_prf_type;
	c->keys_exported = true;

	RSS_DEBUG("DTLS: key export callback (prf=%d secret_len=%zu)", tls_prf_type, secret_len);
}

/* ── Per-client DTLS context ── */

int rwd_dtls_client_init(rwd_client_t *c, rwd_dtls_ctx_t *ctx)
{
	int ret;
	char errbuf[128];

	mbedtls_ssl_init(&c->ssl);

	ret = mbedtls_ssl_setup(&c->ssl, &ctx->conf);
	if (ret != 0) {
		mbedtls_strerror(ret, errbuf, sizeof(errbuf));
		RSS_ERROR("DTLS: ssl_setup failed: %s", errbuf);
		return -1;
	}

	/* Set BIO callbacks */
	mbedtls_ssl_set_bio(&c->ssl, c, rwd_dtls_bio_send, rwd_dtls_bio_recv,
			    rwd_dtls_bio_recv_timeout);

	/* Register key export callback to capture master secret + randoms */
	mbedtls_ssl_set_export_keys_cb(&c->ssl, rwd_dtls_key_export_cb, c);

	/* Set timer callbacks for DTLS retransmission */
	mbedtls_ssl_set_timer_cb(&c->ssl, &c->timer, mbedtls_timing_set_delay,
				 mbedtls_timing_get_delay);

	c->dtls_state = RWD_DTLS_NEW;
	c->dtls_buf_len = 0;

	return 0;
}

void rwd_dtls_client_free(rwd_client_t *c)
{
	mbedtls_ssl_free(&c->ssl);
	c->dtls_state = RWD_DTLS_FAILED;
}

/* ── Drive one step of the DTLS handshake ── */

int rwd_dtls_handshake_step(rwd_client_t *c)
{
	if (c->dtls_state == RWD_DTLS_ESTABLISHED || c->dtls_state == RWD_DTLS_FAILED)
		return c->dtls_state == RWD_DTLS_ESTABLISHED ? 0 : -1;

	/* Set client transport ID for cookie verification */
	if (c->dtls_state == RWD_DTLS_NEW) {
		uint8_t client_id[18]; /* max: 16 bytes IPv6 + 2 bytes port */
		size_t id_len = 0;

		if (c->addr.ss_family == AF_INET) {
			struct sockaddr_in *a = (struct sockaddr_in *)&c->addr;
			memcpy(client_id, &a->sin_addr, 4);
			memcpy(client_id + 4, &a->sin_port, 2);
			id_len = 6;
		} else {
			struct sockaddr_in6 *a = (struct sockaddr_in6 *)&c->addr;
			memcpy(client_id, &a->sin6_addr, 16);
			memcpy(client_id + 16, &a->sin6_port, 2);
			id_len = 18;
		}
		mbedtls_ssl_set_client_transport_id(&c->ssl, client_id, id_len);
		c->dtls_state = RWD_DTLS_HANDSHAKING;
	}

	int ret = mbedtls_ssl_handshake(&c->ssl);

	if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
		return 1; /* in progress, need more data */

	if (ret == MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED) {
		/* Reset for retry after HelloVerifyRequest */
		mbedtls_ssl_session_reset(&c->ssl);
		c->dtls_state = RWD_DTLS_NEW;
		return 1;
	}

	if (ret != 0) {
		char errbuf[128];
		mbedtls_strerror(ret, errbuf, sizeof(errbuf));
		RSS_ERROR("DTLS: handshake failed: %s (0x%04x) state=%d", errbuf, -ret,
			  c->ssl.MBEDTLS_PRIVATE(state));
		c->dtls_state = RWD_DTLS_FAILED;
		return -1;
	}

	/* Handshake complete — verify SRTP profile was negotiated */
	mbedtls_dtls_srtp_info srtp_info;
	mbedtls_ssl_get_dtls_srtp_negotiation_result(&c->ssl, &srtp_info);

	mbedtls_ssl_srtp_profile profile = srtp_info.MBEDTLS_PRIVATE(chosen_dtls_srtp_profile);
	if (profile != MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80 &&
	    profile != MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_32) {
		RSS_ERROR("DTLS: no SRTP profile negotiated");
		c->dtls_state = RWD_DTLS_FAILED;
		return -1;
	}

	c->dtls_state = RWD_DTLS_ESTABLISHED;
	RSS_INFO("DTLS: handshake complete (profile=%d)", profile);
	return 0;
}

/* ── Export SRTP keying material after handshake ── */

int rwd_dtls_export_srtp_keys(rwd_client_t *c, Compy_SrtpKeyMaterial *send_key,
			      Compy_SrtpKeyMaterial *recv_key)
{
	if (c->dtls_state != RWD_DTLS_ESTABLISHED)
		return -1;

	if (!c->keys_exported) {
		RSS_ERROR("DTLS: key export callback was never called");
		return -1;
	}

	uint8_t key_material[SRTP_KEY_MATERIAL_LEN];

	/* RFC 5764 Section 4.2: derive SRTP keys using TLS PRF.
	 * mbedtls_ssl_export_keying_material crashes in this mbedTLS version,
	 * so we use the raw TLS PRF which is equivalent per RFC 5705. */
	static const char label[] = "EXTRACTOR-dtls_srtp";
	int ret = mbedtls_ssl_tls_prf(c->tls_prf, c->master_secret, 48, label, c->randbytes, 64,
				      key_material, SRTP_KEY_MATERIAL_LEN);
	if (ret != 0) {
		char errbuf[128];
		mbedtls_strerror(ret, errbuf, sizeof(errbuf));
		RSS_ERROR("DTLS: failed to derive SRTP keys: %s", errbuf);
		return -1;
	}

	/* Layout (RFC 5764 Section 4.2):
	 * [0..15]  client_write_SRTP_master_key
	 * [16..31] server_write_SRTP_master_key
	 * [32..45] client_write_SRTP_master_salt
	 * [46..59] server_write_SRTP_master_salt
	 *
	 * Try BOTH role assignments — if server keys don't work, use client keys */
	/* Camera is DTLS server → send with server_write, recv with client_write */
	memcpy(send_key->master_key, key_material + SRTP_MASTER_KEY_LEN, SRTP_MASTER_KEY_LEN);
	memcpy(send_key->master_salt, key_material + 2 * SRTP_MASTER_KEY_LEN + SRTP_MASTER_SALT_LEN,
	       SRTP_MASTER_SALT_LEN);

	memcpy(recv_key->master_key, key_material, SRTP_MASTER_KEY_LEN);
	memcpy(recv_key->master_salt, key_material + 2 * SRTP_MASTER_KEY_LEN, SRTP_MASTER_SALT_LEN);

	RSS_INFO("DTLS: SRTP keys exported (prf=%d, send_key=%02x%02x%02x%02x salt=%02x%02x)",
		 c->tls_prf, send_key->master_key[0], send_key->master_key[1],
		 send_key->master_key[2], send_key->master_key[3], send_key->master_salt[0],
		 send_key->master_salt[1]);
	return 0;
}

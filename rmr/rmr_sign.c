/*
 * rmr_sign.c -- fMP4 hash-chain provenance signing
 */

#include "rmr_sign.h"
#include "rmr.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <monocypher.h>

static const uint8_t sign_uuid[16] = RMR_SIGN_UUID;

static int read_urandom(uint8_t *buf, size_t len)
{
	int fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0)
		return -1;
	size_t got = 0;
	while (got < len) {
		ssize_t n = read(fd, buf + got, len - got);
		if (n <= 0) {
			close(fd);
			return -1;
		}
		got += (size_t)n;
	}
	close(fd);
	return 0;
}

static void derive_fingerprint(rmr_sign_key_t *key)
{
	uint8_t digest[64];
	crypto_sha512(digest, key->public, sizeof(key->public));
	memcpy(key->fingerprint, digest, sizeof(key->fingerprint));
	crypto_wipe(digest, sizeof(digest));
}

static int write_file_mode(const char *path, const void *buf, size_t len, mode_t mode)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (fd < 0)
		return -1;
	ssize_t n = write(fd, buf, len);
	int ret = (n == (ssize_t)len && fsync(fd) == 0) ? 0 : -1;
	close(fd);
	return ret;
}

static void mkdir_parent(const char *path)
{
	char dir[256];
	rss_strlcpy(dir, path, sizeof(dir));
	char *slash = strrchr(dir, '/');
	if (slash && slash != dir) {
		*slash = '\0';
		mkdir(dir, 0755);
	}
}

int rmr_sign_key_load(rmr_sign_key_t *key, const char *key_path)
{
	uint8_t seed[32];

	int fd = open(key_path, O_RDONLY);
	if (fd >= 0) {
		ssize_t n = read(fd, seed, sizeof(seed));
		close(fd);
		if (n != (ssize_t)sizeof(seed)) {
			RSS_ERROR("sign key %s: short read (%zd), refusing to sign", key_path, n);
			return -1;
		}
	} else {
		if (read_urandom(seed, sizeof(seed)) < 0) {
			RSS_ERROR("sign key generation: /dev/urandom unavailable");
			return -1;
		}
		mkdir_parent(key_path);
		if (write_file_mode(key_path, seed, sizeof(seed), 0600) < 0) {
			RSS_ERROR("sign key %s: persist failed: %s", key_path, strerror(errno));
			crypto_wipe(seed, sizeof(seed));
			return -1;
		}
		RSS_INFO("generated new signing key: %s", key_path);
	}

	crypto_ed25519_key_pair(key->secret, key->public, seed);
	derive_fingerprint(key);

	/* Convenience hex export of the public key for verifiers */
	char pub_path[256];
	snprintf(pub_path, sizeof(pub_path), "%s.pub", key_path);
	char hex[65];
	for (int i = 0; i < 32; i++)
		snprintf(hex + i * 2, 3, "%02x", key->public[i]);
	if (write_file_mode(pub_path, hex, 64, 0644) < 0)
		RSS_WARN("pubkey export %s failed: %s", pub_path, strerror(errno));

	RSS_INFO("signing key loaded, fingerprint %02x%02x%02x%02x%02x%02x%02x%02x",
		 key->fingerprint[0], key->fingerprint[1], key->fingerprint[2], key->fingerprint[3],
		 key->fingerprint[4], key->fingerprint[5], key->fingerprint[6],
		 key->fingerprint[7]);
	return 0;
}

void rmr_sign_stream_begin(rmr_sign_stream_t *ss, const rmr_sign_key_t *key)
{
	memset(ss, 0, sizeof(*ss));
	ss->key = key;
	crypto_sha512_init(&ss->hash);
}

void rmr_sign_stream_update(rmr_sign_stream_t *ss, const void *buf, uint32_t len)
{
	if (!ss->key || ss->emitting || len == 0)
		return;
	crypto_sha512_update(&ss->hash, buf, len);
	ss->span_bytes += len;
}

int rmr_sign_stream_emit(rmr_sign_stream_t *ss, bool final,
			 int (*write_fn)(const void *, uint32_t, void *), void *ctx)
{
	if (!ss->key || ss->span_bytes == 0)
		return 0;

	uint8_t span_hash[64];
	crypto_sha512_final(&ss->hash, span_hash);

	/* chain input: span hash || previous signature */
	uint8_t chain[128];
	memcpy(chain, span_hash, 64);
	memcpy(chain + 64, ss->prev_sig, 64);

	uint8_t sig[64];
	crypto_ed25519_sign(sig, ss->key->secret, chain, sizeof(chain));

	uint8_t box[RMR_SIGN_BOX_SIZE];
	box[0] = 0;
	box[1] = 0;
	box[2] = (RMR_SIGN_BOX_SIZE >> 8) & 0xff;
	box[3] = RMR_SIGN_BOX_SIZE & 0xff;
	memcpy(box + 4, "uuid", 4);
	memcpy(box + 8, sign_uuid, 16);
	box[24] = 1; /* version */
	box[25] = final ? RMR_SIGN_FLAG_FINAL : 0;
	box[26] = 0;
	box[27] = 0;
	memcpy(box + 28, ss->key->fingerprint, 8);
	memcpy(box + 36, span_hash, 64);
	memcpy(box + 100, sig, 64);

	ss->emitting = true;
	int ret = write_fn(box, sizeof(box), ctx);
	ss->emitting = false;

	memcpy(ss->prev_sig, sig, 64);
	crypto_sha512_init(&ss->hash);
	ss->span_bytes = 0;
	return ret;
}

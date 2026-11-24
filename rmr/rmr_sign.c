/*
 * rmr_sign.c -- fMP4 hash-chain provenance signing
 */

#include "rmr_sign.h"

#include <errno.h>
#include <linux/if_alg.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <rss_common.h>

#ifndef AF_ALG
#define AF_ALG 38
#endif

static const uint8_t sign_uuid[16] = RMR_SIGN_UUID;

static bool kernel_hash = true;
static int alg_tfm = -2; /* -2 not probed, -1 unavailable */

void rmr_sign_use_kernel_hash(bool on)
{
	kernel_hash = on;
}

static int kernel_op(void)
{
	if (!kernel_hash)
		return -1;
	if (alg_tfm == -2) {
		struct sockaddr_alg sa = {
			.salg_family = AF_ALG, .salg_type = "hash", .salg_name = "sha512"};
		alg_tfm = socket(AF_ALG, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
		if (alg_tfm >= 0 && bind(alg_tfm, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
			close(alg_tfm);
			alg_tfm = -1;
		}
		if (alg_tfm < 0)
			alg_tfm = -1;
		RSS_DEBUG("sign: SHA-512 %s", alg_tfm >= 0 ? "in the kernel (AF_ALG)" : "portable");
	}
	if (alg_tfm < 0)
		return -1;
	return accept4(alg_tfm, NULL, NULL, SOCK_CLOEXEC);
}

static void kfd_drop(rmr_sign_stream_t *ss)
{
	if (ss->kfd)
		close(ss->kfd - 1);
	ss->kfd = 0;
	ss->kfd_pending = false;
}

/* The next send() starts a new digest only after the last one was read. */
static void kfd_reset(rmr_sign_stream_t *ss)
{
	uint8_t junk[64];
	if (ss->kfd && ss->kfd_pending &&
	    (send(ss->kfd - 1, NULL, 0, 0) < 0 || read(ss->kfd - 1, junk, sizeof(junk)) != 64))
		kfd_drop(ss);
	ss->kfd_pending = false;
}

void rmr_sign_stream_release(rmr_sign_stream_t *ss)
{
	kfd_drop(ss);
}

void rmr_sign_stream_begin(rmr_sign_stream_t *ss, const rmr_sign_key_t *key)
{
	int kfd = ss->kfd;
	bool pending = ss->kfd_pending;
	memset(ss, 0, sizeof(*ss));
	ss->key = key;
	ss->kfd = kfd;
	ss->kfd_pending = pending;
	kfd_reset(ss);
	if (!ss->kfd) {
		int fd = kernel_op();
		if (fd >= 0)
			ss->kfd = fd + 1;
	}
	crypto_sha512_init(&ss->hash);
}

static void kernel_update(rmr_sign_stream_t *ss, const uint8_t *p, uint32_t len)
{
	while (len) {
		ssize_t n = send(ss->kfd - 1, p, len, MSG_MORE);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			RSS_WARN("sign: kernel SHA-512 failed (%s), this span will not verify",
				 n < 0 ? strerror(errno) : "short send");
			ss->kfd_failed = true;
			kfd_drop(ss);
			return;
		}
		p += n;
		len -= (uint32_t)n;
		ss->kfd_pending = true;
	}
}

void rmr_sign_stream_update(rmr_sign_stream_t *ss, const void *buf, uint32_t len)
{
	if (!ss->key || ss->emitting || len == 0)
		return;
	if (ss->kfd)
		kernel_update(ss, buf, len);
	else
		crypto_sha512_update(&ss->hash, buf, len);
	ss->span_bytes += len;
}

static void span_digest(rmr_sign_stream_t *ss, uint8_t out[64])
{
	if (ss->kfd_failed) {
		/* part of the span never reached a hash: sign something that
		 * cannot match, so the verifier flags this span */
		memset(out, 0, 64);
		ss->kfd_failed = false;
		return;
	}
	if (!ss->kfd) {
		crypto_sha512_final(&ss->hash, out);
		return;
	}
	if (send(ss->kfd - 1, NULL, 0, 0) < 0 || read(ss->kfd - 1, out, 64) != 64) {
		RSS_WARN("sign: kernel SHA-512 digest failed, this span will not verify");
		memset(out, 0, 64);
		kfd_drop(ss);
		return;
	}
	ss->kfd_pending = false;
}

int rmr_sign_stream_emit(rmr_sign_stream_t *ss, bool final,
			 int (*write_fn)(const void *, uint32_t, void *), void *ctx)
{
	if (!ss->key || ss->span_bytes == 0)
		return 0;

	uint8_t span_hash[64];
	span_digest(ss, span_hash);

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

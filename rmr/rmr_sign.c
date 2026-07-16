/*
 * rmr_sign.c -- fMP4 hash-chain provenance signing
 */

#include "rmr_sign.h"

#include <string.h>

static const uint8_t sign_uuid[16] = RMR_SIGN_UUID;

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

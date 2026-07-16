#include "greatest.h"
#include "../rmr/rmr_sign.h"

#include <monocypher-ed25519.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Capture emitted signature boxes into a buffer */
typedef struct {
	uint8_t data[4096];
	uint32_t len;
} capture_t;

static int capture_write(const void *buf, uint32_t len, void *ctx)
{
	capture_t *c = ctx;
	if (c->len + len > sizeof(c->data))
		return -1;
	memcpy(c->data + c->len, buf, len);
	c->len += len;
	return 0;
}

static char key_path[64];

static int make_key(rmr_sign_key_t *key)
{
	char tmpl[] = "/tmp/rmr_sign_test_key_XXXXXX";
	int fd = mkstemp(tmpl);
	if (fd < 0)
		return -1;
	close(fd);
	unlink(tmpl); /* force the generation path */
	snprintf(key_path, sizeof(key_path), "%s", tmpl);
	return rss_sign_key_load(key, key_path);
}

TEST sign_key_generate_and_reload(void)
{
	rmr_sign_key_t k1, k2;
	ASSERT_EQ(0, make_key(&k1));

	/* Reload from the persisted seed — identical key material */
	ASSERT_EQ(0, rss_sign_key_load(&k2, key_path));
	ASSERT_MEM_EQ(k1.public, k2.public, 32);
	ASSERT_MEM_EQ(k1.fingerprint, k2.fingerprint, 8);

	/* Pub export exists and matches */
	char pub_path[300];
	snprintf(pub_path, sizeof(pub_path), "%s.pub", key_path);
	FILE *f = fopen(pub_path, "r");
	ASSERT(f);
	char hex[65] = {0};
	ASSERT_EQ(64u, fread(hex, 1, 64, f));
	fclose(f);
	char expect[65];
	for (int i = 0; i < 32; i++)
		snprintf(expect + i * 2, 3, "%02x", k1.public[i]);
	ASSERT_STR_EQ(expect, hex);

	unlink(key_path);
	unlink(pub_path);
	PASS();
}

TEST sign_chain_verifies(void)
{
	rmr_sign_key_t key;
	ASSERT_EQ(0, make_key(&key));

	static const uint8_t init_seg[] = "ftyp+moov bytes";
	static const uint8_t frag1[] = "fragment one moof+mdat";
	static const uint8_t frag2[] = "fragment two moof+mdat";

	rmr_sign_stream_t ss;
	capture_t cap = {0};

	rmr_sign_stream_begin(&ss, &key);
	rmr_sign_stream_update(&ss, init_seg, sizeof(init_seg));
	ASSERT_EQ(0, rmr_sign_stream_emit(&ss, false, capture_write, &cap));
	rmr_sign_stream_update(&ss, frag1, sizeof(frag1));
	ASSERT_EQ(0, rmr_sign_stream_emit(&ss, false, capture_write, &cap));
	rmr_sign_stream_update(&ss, frag2, sizeof(frag2));
	ASSERT_EQ(0, rmr_sign_stream_emit(&ss, true, capture_write, &cap));

	ASSERT_EQ(3u * RMR_SIGN_BOX_SIZE, cap.len);

	/* Independently verify the chain the way rverify does */
	const uint8_t *spans[3] = {init_seg, frag1, frag2};
	const size_t span_lens[3] = {sizeof(init_seg), sizeof(frag1), sizeof(frag2)};
	uint8_t prev_sig[64] = {0};

	for (int i = 0; i < 3; i++) {
		const uint8_t *box = cap.data + (size_t)i * RMR_SIGN_BOX_SIZE;
		const uint8_t sign_uuid[16] = RMR_SIGN_UUID;

		ASSERT_EQ(0, memcmp(box + 4, "uuid", 4));
		ASSERT_EQ(0, memcmp(box + 8, sign_uuid, 16));
		ASSERT_EQ(1, box[24]);
		ASSERT_EQ(i == 2 ? RMR_SIGN_FLAG_FINAL : 0, box[25]);
		ASSERT_MEM_EQ(key.fingerprint, box + 28, 8);

		uint8_t span_hash[64];
		crypto_sha512(span_hash, spans[i], span_lens[i]);
		ASSERT_MEM_EQ(span_hash, box + 36, 64);

		uint8_t chain[128];
		memcpy(chain, span_hash, 64);
		memcpy(chain + 64, prev_sig, 64);
		ASSERT_EQ(0, crypto_ed25519_check(box + 100, key.public, chain, sizeof(chain)));

		memcpy(prev_sig, box + 100, 64);
	}

	/* Tampered span fails hash comparison */
	uint8_t bad[sizeof(frag1)];
	memcpy(bad, frag1, sizeof(frag1));
	bad[3] ^= 0x01;
	uint8_t bad_hash[64];
	crypto_sha512(bad_hash, bad, sizeof(bad));
	ASSERT(memcmp(bad_hash, cap.data + RMR_SIGN_BOX_SIZE + 36, 64) != 0);

	/* Wrong key fails signature check */
	rmr_sign_key_t other;
	ASSERT_EQ(0, make_key(&other));
	uint8_t h0[64];
	crypto_sha512(h0, init_seg, sizeof(init_seg));
	uint8_t chain0[128] = {0};
	memcpy(chain0, h0, 64);
	ASSERT(crypto_ed25519_check(cap.data + 100, other.public, chain0, sizeof(chain0)) != 0);

	char pub_path[300];
	snprintf(pub_path, sizeof(pub_path), "%s.pub", key_path);
	unlink(key_path);
	unlink(pub_path);
	PASS();
}

TEST sign_empty_span_is_noop(void)
{
	rmr_sign_key_t key;
	ASSERT_EQ(0, make_key(&key));

	rmr_sign_stream_t ss;
	capture_t cap = {0};
	rmr_sign_stream_begin(&ss, &key);
	ASSERT_EQ(0, rmr_sign_stream_emit(&ss, false, capture_write, &cap));
	ASSERT_EQ(0u, cap.len);

	/* update during emit is suppressed */
	ss.emitting = true;
	rmr_sign_stream_update(&ss, "x", 1);
	ss.emitting = false;
	ASSERT_EQ(0u, (unsigned)ss.span_bytes);

	char pub_path[300];
	snprintf(pub_path, sizeof(pub_path), "%s.pub", key_path);
	unlink(key_path);
	unlink(pub_path);
	PASS();
}

SUITE(sign_suite)
{
	RUN_TEST(sign_key_generate_and_reload);
	RUN_TEST(sign_chain_verifies);
	RUN_TEST(sign_empty_span_is_noop);
}

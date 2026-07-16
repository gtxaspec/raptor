/*
 * rmr_sign.h -- fMP4 hash-chain provenance signing
 *
 * Every recorded file carries a chain of Ed25519 signatures in uuid
 * boxes. Each box signs the SHA-512 of all file bytes since the
 * previous box (the "span") chained to the previous signature, so
 * removing, reordering, or altering any byte of any span breaks the
 * chain. Spans are: the init segment (genesis box after ftyp+moov),
 * each fragment group (box after moof+mdat), and the mfra tail
 * (final box, flagged, proves clean close). Files cut by power loss
 * verify up to the last complete box.
 *
 * Box layout (164 bytes):
 *   4  size          8+16+4+8+64+64, big-endian
 *   4  'uuid'
 *   16 RMR_SIGN_UUID
 *   1  version (1)
 *   1  flags (bit 0: final box)
 *   2  reserved (0)
 *   8  key fingerprint (first 8 bytes of SHA-512 of the public key)
 *   64 SHA-512 of the span
 *   64 Ed25519 signature over hash || prev_signature
 *
 * The genesis box uses 64 zero bytes as prev_signature.
 */

#ifndef RMR_SIGN_H
#define RMR_SIGN_H

#include <stdbool.h>
#include <stdint.h>

#include <monocypher-ed25519.h>
#include <rss_sign.h>

#define RMR_SIGN_BOX_SIZE   164
#define RMR_SIGN_FLAG_FINAL 0x01

/* uuid box identifier for raptor signature boxes (fixed forever) */
#define RMR_SIGN_UUID                                                                              \
	{0xb0, 0x50, 0xbf, 0x07, 0x88, 0x56, 0x45, 0x98,                                           \
	 0x86, 0x9e, 0x44, 0xc4, 0xdc, 0x20, 0x8f, 0x26}

/* Device key (rss_sign.h; loaded once, shared by all signing streams) */
typedef rss_sign_key_t rmr_sign_key_t;

/* Per-file signing stream */
typedef struct {
	const rmr_sign_key_t *key;
	crypto_sha512_ctx hash; /* running hash of current span   */
	uint8_t prev_sig[64];
	uint64_t span_bytes; /* bytes hashed in current span      */
	bool emitting;	     /* suppress update() during box write */
} rmr_sign_stream_t;

/* Begin signing a new file (call once per segment/clip open). */
void rmr_sign_stream_begin(rmr_sign_stream_t *ss, const rmr_sign_key_t *key);

/* Hash bytes written to the file. Call from the write callback. */
void rmr_sign_stream_update(rmr_sign_stream_t *ss, const void *buf, uint32_t len);

/*
 * Sign the current span and emit the signature box through write_fn.
 * No-op (returns 0) when the span is empty. final marks the closing
 * box. Returns write_fn's result.
 */
int rmr_sign_stream_emit(rmr_sign_stream_t *ss, bool final,
			 int (*write_fn)(const void *, uint32_t, void *), void *ctx);

#endif /* RMR_SIGN_H */

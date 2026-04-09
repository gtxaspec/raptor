/*
 * Fuzz target for STUN message parsing.
 *
 * Exercises the header validation and attribute walker from rwd_ice.c
 * without needing a full server context. Extracts the same parsing
 * logic that rwd_ice_process uses before the client lookup.
 *
 * Build:  make -C fuzz
 * Run:    ./fuzz/fuzz_stun corpus/stun/
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

/* STUN constants (from rwd.h) */
#define STUN_HEADER_SIZE    20
#define STUN_MAGIC_COOKIE   0x2112A442
#define STUN_BINDING_REQUEST 0x0001
#define STUN_ATTR_USERNAME  0x0006
#define STUN_ATTR_MSG_INTEGRITY 0x0008

static inline uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
}

/* Attribute walker — same logic as stun_get_username in rwd_ice.c */
static int stun_walk_attrs(const uint8_t *msg, size_t msg_len,
			   char *username, size_t username_size)
{
	const uint8_t *p = msg + STUN_HEADER_SIZE;
	const uint8_t *end = msg + msg_len;

	while (p + 4 <= end) {
		uint16_t attr_type = rd16(p);
		uint16_t attr_len = rd16(p + 2);
		size_t padded = (attr_len + 3) & ~3u;

		if (p + 4 + attr_len > end)
			break;

		if (attr_type == STUN_ATTR_USERNAME) {
			if (attr_len >= username_size)
				return -1;
			memcpy(username, p + 4, attr_len);
			username[attr_len] = '\0';
			return 0;
		}
		if (p + 4 + padded > end)
			break;
		p += 4 + padded;
	}
	return -1;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size < STUN_HEADER_SIZE)
		return 0;

	/* Header validation — same as rwd_ice_process */
	uint16_t msg_type = rd16(data);
	uint16_t msg_len = rd16(data + 2);
	uint32_t cookie = rd32(data + 4);

	if (cookie != STUN_MAGIC_COOKIE)
		return 0;
	if (msg_type != STUN_BINDING_REQUEST)
		return 0;
	if ((size_t)(STUN_HEADER_SIZE + msg_len) > size)
		return 0;

	/* Walk attributes */
	char username[128];
	(void)stun_walk_attrs(data, STUN_HEADER_SIZE + msg_len,
			      username, sizeof(username));

	return 0;
}

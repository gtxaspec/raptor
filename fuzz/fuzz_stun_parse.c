/*
 * Fuzz target for STUN message parsing.
 *
 * Calls the production rwd_ice_process with a minimal server context,
 * exercising the real attribute walker, MESSAGE-INTEGRITY verification
 * (hmac_buf sizing, mi_offset math), CRC32 fingerprint, and response
 * builder — not a reimplementation.
 *
 * Build:  make -C fuzz fuzz_stun
 * Run:    ./fuzz/fuzz_stun corpus/stun/
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>
#include <netinet/in.h>

#include "fuzz_stun_shim.h"

/* Stub HMAC — fills output with zeros. We don't need correct crypto
 * for memory safety fuzzing, just need the code paths to execute. */
void rwd_hmac_sha1(const uint8_t *key, size_t key_len,
		   const uint8_t *data, size_t data_len, uint8_t *out)
{
	(void)key;
	(void)key_len;
	(void)data;
	(void)data_len;
	memset(out, 0, 20);
}

static rwd_server_t srv;
static rwd_client_t client;
static bool initialized;

static void init_once(void)
{
	if (initialized)
		return;
	initialized = true;

	rwd_crc32_init();
	pthread_mutex_init(&srv.clients_lock, NULL);
	srv.udp_fd = -1; /* sendto harmlessly returns EBADF */

	memset(&client, 0, sizeof(client));
	snprintf(client.local_ufrag, sizeof(client.local_ufrag), "fuzz");
	snprintf(client.local_pwd, sizeof(client.local_pwd), "fuzzpassword1234");
	client.active = true;
	snprintf(client.session_id, sizeof(client.session_id), "fuzz-session");

	srv.clients[0] = &client;
	srv.client_count = 1;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	init_once();

	struct sockaddr_in from = {
		.sin_family = AF_INET,
		.sin_port = htons(12345),
		.sin_addr.s_addr = htonl(0x0A190101),
	};

	/* Reset client state between runs so ice_verified path varies */
	client.ice_verified = false;

	rwd_ice_process(&srv, data, size,
			(const struct sockaddr_storage *)&from, sizeof(from));

	return 0;
}

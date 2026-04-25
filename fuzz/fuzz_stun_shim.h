/*
 * Minimal rwd.h replacement for the STUN fuzzer.
 *
 * Defines only the struct fields that rwd_ice.c accesses so we can
 * compile and call rwd_ice_process without pulling in compy/mbedTLS.
 */

#ifndef FUZZ_STUN_SHIM_H
#define FUZZ_STUN_SHIM_H

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/socket.h>
#include <rss_common.h>

#define RWD_MAX_CLIENTS		    4
#define RWD_SESSION_ID_LEN	    16
#define RWD_UDP_BUF_SIZE	    2048

#define STUN_HEADER_SIZE	    20
#define STUN_MAGIC_COOKIE	    0x2112A442
#define STUN_BINDING_REQUEST	    0x0001
#define STUN_BINDING_RESPONSE	    0x0101
#define STUN_ATTR_USERNAME	    0x0006
#define STUN_ATTR_MESSAGE_INTEGRITY 0x0008
#define STUN_ATTR_XOR_MAPPED_ADDR   0x0020
#define STUN_ATTR_FINGERPRINT	    0x8028
#define STUN_FINGERPRINT_XOR	    0x5354554E

typedef struct rwd_client rwd_client_t;
typedef struct rwd_server rwd_server_t;

struct rwd_client {
	struct sockaddr_storage addr;
	socklen_t addr_len;
	bool active;

	char local_ufrag[16];
	char local_pwd[32];
	char remote_ufrag[64];
	char remote_pwd[256];
	bool ice_verified;
	int64_t last_stun_at;

	char session_id[RWD_SESSION_ID_LEN * 2 + 1];
};

struct rwd_server {
	int udp_fd;
	int http_fd;
	int epoll_fd;
	void *ctrl;
	void *cfg;
	const char *config_path;
	volatile sig_atomic_t *running;
	void *dtls;

	rwd_client_t *clients[RWD_MAX_CLIENTS];
	int client_count;
	pthread_mutex_t clients_lock;
};

void rwd_crc32_init(void);
int rwd_ice_process(rwd_server_t *srv, const uint8_t *buf, size_t len,
		    const struct sockaddr_storage *from, socklen_t from_len);

void rwd_hmac_sha1(const uint8_t *key, size_t key_len,
		   const uint8_t *data, size_t data_len, uint8_t *out);

#define ICE_PRIORITY_SRFLX 0x6E001EFF

static inline void rwd_random_bytes(uint8_t *buf, size_t len)
{
	for (size_t i = 0; i < len; i++)
		buf[i] = (uint8_t)i;
}

#endif

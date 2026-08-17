#ifndef RWD_PACER_H
#define RWD_PACER_H

#include <stdint.h>

typedef struct {
	uint32_t rate_bps;
	uint32_t burst_bytes;
	uint64_t tokens;
	int64_t last_us;
} rwd_pacer_t;

void rwd_pacer_init(rwd_pacer_t *pacer, uint32_t rate_bps, uint32_t burst_bytes, int64_t now_us);
uint64_t rwd_pacer_reserve(rwd_pacer_t *pacer, uint32_t bytes, int64_t now_us);

#endif

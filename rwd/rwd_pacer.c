#include "rwd_pacer.h"

#define BITS_PER_BYTE 8ULL
#define US_PER_SECOND 1000000ULL

void rwd_pacer_init(rwd_pacer_t *pacer, uint32_t rate_bps, uint32_t burst_bytes, int64_t now_us)
{
	pacer->rate_bps = rate_bps;
	pacer->burst_bytes = burst_bytes;
	pacer->tokens = burst_bytes;
	pacer->last_us = now_us;
}

static void rwd_pacer_refill(rwd_pacer_t *pacer, int64_t now_us)
{
	if (now_us <= pacer->last_us)
		return;

	uint64_t elapsed_us = (uint64_t)(now_us - pacer->last_us);
	uint64_t room = pacer->burst_bytes - pacer->tokens;
	uint64_t fill_us =
		(room * BITS_PER_BYTE * US_PER_SECOND + pacer->rate_bps - 1) / pacer->rate_bps;
	if (elapsed_us >= fill_us) {
		pacer->tokens = pacer->burst_bytes;
		pacer->last_us = now_us;
		return;
	}
	uint64_t added = elapsed_us * pacer->rate_bps / (BITS_PER_BYTE * US_PER_SECOND);
	if (added >= room)
		pacer->tokens = pacer->burst_bytes;
	else
		pacer->tokens += added;
	pacer->last_us = now_us;
}

uint64_t rwd_pacer_reserve(rwd_pacer_t *pacer, uint32_t bytes, int64_t now_us)
{
	if (!pacer->rate_bps || !pacer->burst_bytes || !bytes)
		return 0;

	if (bytes > pacer->burst_bytes)
		bytes = pacer->burst_bytes;
	rwd_pacer_refill(pacer, now_us);

	if (pacer->tokens >= bytes) {
		pacer->tokens -= bytes;
		return 0;
	}

	/* Refill a complete burst instead of sleeping once per RTP packet. This
	 * releases short groups of datagrams with a bounded gap between groups,
	 * which is both cheaper on the embedded scheduler and gentle on the Wi-Fi
	 * transmit queue. */
	uint64_t missing = pacer->burst_bytes - pacer->tokens;
	uint64_t numerator = missing * BITS_PER_BYTE * US_PER_SECOND;
	uint64_t wait_us = (numerator + pacer->rate_bps - 1) / pacer->rate_bps;

	pacer->last_us = now_us + (int64_t)wait_us;
	pacer->tokens = pacer->burst_bytes - bytes;
	return wait_us;
}

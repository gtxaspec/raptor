/*
 * Fuzz target for rwd_sdp_parse_offer.
 *
 * Compiles rwd_sdp.c standalone by providing a minimal shim header
 * instead of the full rwd.h (which pulls in compy + mbedTLS).
 *
 * Build:  make -C fuzz
 * Run:    ./fuzz/fuzz_sdp corpus/sdp/
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "fuzz_rwd_shim.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	/* SDP parser expects null-terminated string */
	char *sdp = malloc(size + 1);
	if (!sdp)
		return 0;
	memcpy(sdp, data, size);
	sdp[size] = '\0';

	rwd_sdp_offer_t offer;
	(void)rwd_sdp_parse_offer(sdp, &offer);

	free(sdp);
	return 0;
}

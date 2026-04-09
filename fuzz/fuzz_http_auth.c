/*
 * Fuzz target for HTTP Basic auth parsing (base64 decode + header extract).
 *
 * Build:  make -C fuzz
 * Run:    ./fuzz/fuzz_http_auth corpus/http_auth/
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include <rss_http.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	/* Auth parser expects null-terminated HTTP request string */
	char *req = malloc(size + 1);
	if (!req)
		return 0;
	memcpy(req, data, size);
	req[size] = '\0';

	rss_base64_init();
	(void)rss_http_check_basic_auth(req, "admin", "secret");

	free(req);
	return 0;
}

/*
 * rverify -- verify signed raptor recordings
 *
 * Walks the top-level boxes of an fMP4 file produced by RMR with
 * [recording] sign enabled. Every raptor signature uuid box covers
 * the SHA-512 of all bytes since the previous signature box, chained
 * to the previous Ed25519 signature (see rmr/rmr_sign.h). Any edit,
 * removal, or reordering of signed bytes breaks the chain.
 *
 * With a public key (-k) each signature is verified; without one the
 * tool still checks hash consistency and reports the key fingerprint
 * needed. -t additionally dumps embedded ST 0604 timecodes.
 *
 * JPEG input (RHD signed snapshots) verifies the trailing APP15
 * signature segment and reports the EXIF capture time instead.
 *
 * Exit codes: 0 verified clean, 1 verification failure, 2 usage or
 * file has no signatures.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <monocypher-ed25519.h>
#include <rss_jpeg.h>

#include "../rmr/rmr_sign.h"

static const uint8_t sign_uuid[16] = RMR_SIGN_UUID;
static const uint8_t misp_uuid_h264[16] = {'M', 'I', 'S', 'P', 'm', 'i', 'c', 'r',
					   'o', 's', 'e', 'c', 't', 'i', 'm', 'e'};
static const uint8_t misp_uuid_h265[16] = {0xa8, 0x68, 0x7d, 0xd4, 0xd7, 0x59, 0x37, 0x58,
					   0xa5, 0xce, 0xf0, 0x33, 0x8b, 0x65, 0x45, 0xf1};

static int hex_nibble(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static int parse_pubkey(const char *arg, uint8_t pub[32])
{
	char hex[65];

	/* Accept a 64-char hex string or a file containing one */
	if (strlen(arg) == 64) {
		memcpy(hex, arg, 64);
	} else {
		FILE *f = fopen(arg, "r");
		if (!f) {
			fprintf(stderr, "rverify: %s: %s\n", arg, strerror(errno));
			return -1;
		}
		size_t n = fread(hex, 1, 64, f);
		fclose(f);
		if (n != 64) {
			fprintf(stderr, "rverify: %s: expected 64 hex chars\n", arg);
			return -1;
		}
	}
	hex[64] = '\0';

	for (int i = 0; i < 32; i++) {
		int hi = hex_nibble(hex[i * 2]);
		int lo = hex_nibble(hex[i * 2 + 1]);
		if (hi < 0 || lo < 0) {
			fprintf(stderr, "rverify: invalid hex in public key\n");
			return -1;
		}
		pub[i] = (uint8_t)((hi << 4) | lo);
	}
	return 0;
}

static void fmt_utc(uint64_t utc_us, char *buf, size_t cap)
{
	time_t sec = (time_t)(utc_us / 1000000);
	struct tm tm;
	gmtime_r(&sec, &tm);
	size_t n = strftime(buf, cap, "%Y-%m-%dT%H:%M:%S", &tm);
	snprintf(buf + n, cap - n, ".%06uZ", (unsigned)(utc_us % 1000000));
}

/*
 * Scan the file for embedded ST 0604 timecode SEIs. Structural match
 * (SEI header + identifier) rather than a full moof/trun parse —
 * sufficient for reporting, the signature chain covers the bytes.
 */
static void dump_timecodes(const uint8_t *data, size_t len, bool verbose)
{
	uint64_t first = 0, last = 0, prev = 0;
	unsigned count = 0, backwards = 0;

	for (size_t i = 0; i + 16 + 12 <= len; i++) {
		bool h264 = !memcmp(data + i, misp_uuid_h264, 16);
		bool h265 = !h264 && !memcmp(data + i, misp_uuid_h265, 16);
		if (!h264 && !h265)
			continue;
		/* preceding bytes must be the SEI header: 06 05 1c (H.264)
		 * or 4e 01 05 1c (H.265) */
		if (h264 && (i < 3 || memcmp(data + i - 3, "\x06\x05\x1c", 3) != 0))
			continue;
		if (h265 && (i < 4 || memcmp(data + i - 4, "\x4e\x01\x05\x1c", 4) != 0))
			continue;

		const uint8_t *d = data + i + 16;
		if (d[3] != 0xff || d[6] != 0xff || d[9] != 0xff)
			continue;
		uint64_t utc = ((uint64_t)d[1] << 56) | ((uint64_t)d[2] << 48) |
			       ((uint64_t)d[4] << 40) | ((uint64_t)d[5] << 32) |
			       ((uint64_t)d[7] << 24) | ((uint64_t)d[8] << 16) |
			       ((uint64_t)d[10] << 8) | (uint64_t)d[11];

		if (count == 0)
			first = utc;
		else if (utc < prev)
			backwards++;
		prev = utc;
		last = utc;
		count++;

		if (verbose) {
			char ts[40];
			fmt_utc(utc, ts, sizeof(ts));
			printf("  frame %5u  %s  status 0x%02x %s\n", count, ts, d[0],
			       (d[0] & 0x80) ? "(lock unknown)" : "(locked)");
		}
	}

	if (count == 0) {
		printf("timecodes: none\n");
		return;
	}
	char f[40], l[40];
	fmt_utc(first, f, sizeof(f));
	fmt_utc(last, l, sizeof(l));
	printf("timecodes: %u frames, %s .. %s%s\n", count, f, l,
	       backwards ? " (WARNING: non-monotonic)" : "");
}

/* Signed JPEG snapshot (RHD): EXIF capture time + APP15 signature */
static int verify_jpeg(const uint8_t *data, size_t len, const uint8_t *pub)
{
	uint64_t utc;
	if (rss_jpeg_get_exif_time(data, len, &utc) == 0) {
		char ts[40];
		fmt_utc(utc, ts, sizeof(ts));
		printf("capture time: %s (EXIF)\n", ts);
	}

	uint8_t fp[8];
	int rc = rss_jpeg_verify(data, len, pub, fp);
	if (rc == -ENOENT) {
		printf("no raptor signature segment found\n");
		return 2;
	}
	if (rc == 0 || rc == 1)
		printf("key fingerprint: %02x%02x%02x%02x%02x%02x%02x%02x\n", fp[0], fp[1], fp[2],
		       fp[3], fp[4], fp[5], fp[6], fp[7]);
	if (rc == 1) {
		printf("RESULT: SIGNED (supply -k to verify)\n");
		return 0;
	}
	if (rc == 0) {
		printf("RESULT: VERIFIED\n");
		return 0;
	}
	printf("RESULT: TAMPERED\n");
	return 1;
}

int main(int argc, char **argv)
{
	const char *key_arg = NULL;
	const char *path = NULL;
	bool dump_tc = false;
	bool verbose_tc = false;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-k") && i + 1 < argc)
			key_arg = argv[++i];
		else if (!strcmp(argv[i], "-t"))
			dump_tc = true;
		else if (!strcmp(argv[i], "-T"))
			dump_tc = verbose_tc = true;
		else if (argv[i][0] == '-')
			goto usage;
		else
			path = argv[i];
	}
	if (!path) {
	usage:
		fprintf(stderr,
			"usage: rverify [-k pubkey_hex|pubkey_file] [-t|-T] file.{mp4,jpg}\n"
			"  -k  verify signatures against this Ed25519 public key\n"
			"  -t  summarize embedded ST 0604 timecodes (mp4)\n"
			"  -T  dump every timecode (mp4)\n"
			"JPEG input: verifies the RHD snapshot signature and\n"
			"reports the EXIF capture time.\n");
		return 2;
	}

	uint8_t pub[32];
	bool have_key = false;
	if (key_arg) {
		if (parse_pubkey(key_arg, pub) < 0)
			return 2;
		have_key = true;
	}

	FILE *f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "rverify: %s: %s\n", path, strerror(errno));
		return 2;
	}
	fseek(f, 0, SEEK_END);
	long fsize = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (fsize <= 0) {
		fclose(f);
		fprintf(stderr, "rverify: %s: empty file\n", path);
		return 2;
	}
	uint8_t *data = malloc((size_t)fsize);
	if (!data || fread(data, 1, (size_t)fsize, f) != (size_t)fsize) {
		fprintf(stderr, "rverify: %s: read failed\n", path);
		free(data);
		fclose(f);
		return 2;
	}
	fclose(f);
	size_t len = (size_t)fsize;

	/* JPEG snapshot? Verify the APP15 signature instead of a box walk */
	if (len >= 4 && data[0] == 0xff && data[1] == 0xd8) {
		int rc = verify_jpeg(data, len, have_key ? pub : NULL);
		free(data);
		return rc;
	}

	/* Walk top-level boxes, verifying each signed span */
	uint8_t prev_sig[64] = {0};
	size_t span_start = 0;
	size_t pos = 0;
	unsigned box_count = 0, failures = 0;
	bool saw_final = false;

	while (pos + 8 <= len) {
		uint64_t box_size = ((uint64_t)data[pos] << 24) | ((uint64_t)data[pos + 1] << 16) |
				    ((uint64_t)data[pos + 2] << 8) | (uint64_t)data[pos + 3];
		const uint8_t *type = data + pos + 4;
		uint64_t hdr = 8;

		if (box_size == 1) { /* 64-bit largesize */
			if (pos + 16 > len)
				break;
			box_size = 0;
			for (int i = 0; i < 8; i++)
				box_size = (box_size << 8) | data[pos + 8 + i];
			hdr = 16;
		} else if (box_size == 0) { /* extends to EOF */
			box_size = len - pos;
		}
		if (box_size < hdr || box_size > len - pos)
			break; /* truncated/corrupt box — trailing data */

		if (!memcmp(type, "uuid", 4) && box_size == RMR_SIGN_BOX_SIZE &&
		    pos + RMR_SIGN_BOX_SIZE <= len && !memcmp(data + pos + 8, sign_uuid, 16)) {
			const uint8_t *box = data + pos;
			uint8_t version = box[24];
			uint8_t flags = box[25];
			const uint8_t *fp = box + 28;
			const uint8_t *stored_hash = box + 36;
			const uint8_t *sig = box + 100;
			box_count++;

			if (saw_final) {
				printf("box %2u @ %8zu: FAIL — data after final box\n", box_count,
				       pos);
				failures++;
			}

			if (box_count == 1)
				printf("key fingerprint: "
				       "%02x%02x%02x%02x%02x%02x%02x%02x\n",
				       fp[0], fp[1], fp[2], fp[3], fp[4], fp[5], fp[6], fp[7]);

			uint8_t span_hash[64];
			crypto_sha512(span_hash, data + span_start, pos - span_start);

			bool hash_ok = !memcmp(span_hash, stored_hash, 64);
			bool sig_ok = true;
			if (have_key) {
				uint8_t chain[128];
				memcpy(chain, stored_hash, 64);
				memcpy(chain + 64, prev_sig, 64);
				sig_ok = crypto_ed25519_check(sig, pub, chain, sizeof(chain)) == 0;
			}

			const char *verdict = (!hash_ok)   ? "FAIL — span hash mismatch"
					      : (!sig_ok)  ? "FAIL — bad signature"
					      : (have_key) ? "OK"
							   : "hash ok (no key)";
			printf("box %2u @ %8zu: span %8zu bytes  v%u%s  %s\n", box_count, pos,
			       pos - span_start, version,
			       (flags & RMR_SIGN_FLAG_FINAL) ? " final" : "", verdict);
			if (!hash_ok || !sig_ok)
				failures++;
			if (flags & RMR_SIGN_FLAG_FINAL)
				saw_final = true;

			memcpy(prev_sig, sig, 64);
			span_start = pos + box_size;
		}

		pos += box_size;
	}

	if (box_count == 0) {
		printf("no raptor signature boxes found\n");
		if (dump_tc)
			dump_timecodes(data, len, verbose_tc);
		free(data);
		return 2;
	}

	size_t trailing = len - span_start;
	if (trailing > 0) {
		if (saw_final) {
			printf("FAIL — %zu unsigned bytes after final box\n", trailing);
			failures++;
		} else {
			printf("note: %zu trailing unsigned bytes (incomplete last fragment)\n",
			       trailing);
		}
	}
	if (!saw_final && trailing == 0)
		printf("note: no final box — file was not cleanly closed\n");

	if (dump_tc)
		dump_timecodes(data, len, verbose_tc);

	if (failures) {
		printf("RESULT: TAMPERED (%u failure%s, %u box%s)\n", failures,
		       failures == 1 ? "" : "s", box_count, box_count == 1 ? "" : "es");
		free(data);
		return 1;
	}
	printf("RESULT: %s (%u box%s%s)\n",
	       have_key ? "VERIFIED" : "CONSISTENT (supply -k to verify)", box_count,
	       box_count == 1 ? "" : "es", saw_final ? ", cleanly closed" : "");
	free(data);
	return 0;
}

#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/*
 * Simulated safe version of the vulnerable memcpy pattern from rsr_main.c
 * The invariant: memcpy(adts_buf + 7, audio_buf, alen) must never write
 * beyond adts_buf's allocated size.
 *
 * Safe implementation enforces: alen <= (adts_buf_size - 7)
 * If alen exceeds this, the copy must be rejected or truncated.
 */

#define ADTS_HEADER_SIZE 7
#define ADTS_BUF_SIZE    1024  /* typical allocation size */

/*
 * Safe wrapper that enforces the invariant:
 * Returns 0 on success, -1 if alen would cause overflow.
 */
static int safe_adts_copy(uint8_t *adts_buf, size_t adts_buf_size,
                           const uint8_t *audio_buf, size_t alen)
{
    /* Invariant check: alen must not exceed available space after header */
    if (alen > adts_buf_size - ADTS_HEADER_SIZE) {
        return -1; /* reject oversized input */
    }
    if (adts_buf_size < ADTS_HEADER_SIZE) {
        return -1; /* buffer too small to even hold header */
    }
    memcpy(adts_buf + ADTS_HEADER_SIZE, audio_buf, alen);
    return 0;
}

START_TEST(test_buffer_read_never_exceeds_declared_length)
{
    /* Invariant: memcpy of audio_buf into adts_buf+7 must never exceed
     * the allocated size of adts_buf. Oversized inputs must be rejected. */

    /* Each payload entry: {data_size, description} */
    struct {
        size_t alen;
        const char *description;
    } payloads[] = {
        /* Normal boundary cases */
        { 0,                          "zero length" },
        { 1,                          "single byte" },
        { ADTS_BUF_SIZE - ADTS_HEADER_SIZE - 1, "one byte under limit" },
        { ADTS_BUF_SIZE - ADTS_HEADER_SIZE,     "exact limit" },

        /* Oversized: should be rejected */
        { ADTS_BUF_SIZE - ADTS_HEADER_SIZE + 1, "one byte over limit" },
        { ADTS_BUF_SIZE,                         "full buf size (overflow by 7)" },
        { ADTS_BUF_SIZE * 2,                     "2x buffer size" },
        { ADTS_BUF_SIZE * 10,                    "10x buffer size" },
        { 65535,                                  "max uint16 (typical audio frame max)" },
        { 65536,                                  "exceed uint16" },
        { 0x7FFFFFFF,                             "large value near INT_MAX" },
        { SIZE_MAX,                               "SIZE_MAX overflow attempt" },
        { SIZE_MAX - 6,                           "SIZE_MAX - 6 (wraps past header offset)" },
        { SIZE_MAX - 7,                           "SIZE_MAX - 7 (exact wrap)" },
        { 0xDEADBEEF,                             "adversarial magic value" },
        { 0xCAFEBABE,                             "adversarial magic value 2" },
        { ADTS_BUF_SIZE + 1,                      "buf_size + 1" },
        { ADTS_BUF_SIZE * 100,                    "100x buffer size" },
        { 4096,                                   "4096 bytes (4x typical limit)" },
        { 8192,                                   "8192 bytes (8x typical limit)" },
    };

    int num_payloads = sizeof(payloads) / sizeof(payloads[0]);

    for (int i = 0; i < num_payloads; i++) {
        size_t alen = payloads[i].alen;

        /* Allocate adts_buf with known size */
        uint8_t *adts_buf = (uint8_t *)malloc(ADTS_BUF_SIZE);
        ck_assert_msg(adts_buf != NULL, "Failed to allocate adts_buf");

        /* Allocate a safe audio_buf — we only allocate min(alen, ADTS_BUF_SIZE*10)
         * to avoid exhausting memory, but we test the invariant via the safe wrapper */
        size_t safe_audio_alloc = (alen < ADTS_BUF_SIZE * 10 && alen != SIZE_MAX &&
                                   alen != SIZE_MAX - 6 && alen != SIZE_MAX - 7 &&
                                   alen != 0x7FFFFFFF && alen != 0xDEADBEEF &&
                                   alen != 0xCAFEBABE)
                                  ? alen : ADTS_BUF_SIZE;
        if (safe_audio_alloc == 0) safe_audio_alloc = 1;

        uint8_t *audio_buf = (uint8_t *)calloc(safe_audio_alloc, 1);
        ck_assert_msg(audio_buf != NULL, "Failed to allocate audio_buf");

        /* Fill with adversarial pattern */
        memset(audio_buf, 0xAA, safe_audio_alloc);

        /* Initialize adts_buf with known sentinel values */
        memset(adts_buf, 0xBB, ADTS_BUF_SIZE);

        int result = safe_adts_copy(adts_buf, ADTS_BUF_SIZE, audio_buf, alen);

        if (alen <= ADTS_BUF_SIZE - ADTS_HEADER_SIZE) {
            /* Should succeed: alen fits within buffer */
            ck_assert_msg(result == 0,
                "FAIL [%s]: alen=%zu should fit in buf (max=%zu) but was rejected",
                payloads[i].description, alen,
                (size_t)(ADTS_BUF_SIZE - ADTS_HEADER_SIZE));

            /* Verify header area is untouched */
            for (int j = 0; j < ADTS_HEADER_SIZE; j++) {
                ck_assert_msg(adts_buf[j] == 0xBB,
                    "FAIL [%s]: header byte %d was overwritten",
                    payloads[i].description, j);
            }

            /* Verify copied data is correct */
            if (alen > 0) {
                ck_assert_msg(
                    memcmp(adts_buf + ADTS_HEADER_SIZE, audio_buf, alen) == 0,
                    "FAIL [%s]: copied data mismatch",
                    payloads[i].description);
            }
        } else {
            /* Should be rejected: alen would overflow the buffer */
            ck_assert_msg(result == -1,
                "FAIL [%s]: alen=%zu exceeds buffer capacity (%zu) but was NOT rejected — "
                "heap buffer overflow would occur!",
                payloads[i].description, alen,
                (size_t)(ADTS_BUF_SIZE - ADTS_HEADER_SIZE));

            /* Verify adts_buf is completely untouched after rejection */
            for (size_t j = 0; j < ADTS_BUF_SIZE; j++) {
                ck_assert_msg(adts_buf[j] == 0xBB,
                    "FAIL [%s]: adts_buf[%zu] was modified despite rejection",
                    payloads[i].description, j);
            }
        }

        free(adts_buf);
        free(audio_buf);
    }
}
END_TEST

START_TEST(test_adts_header_offset_boundary)
{
    /* Invariant: The 7-byte ADTS header offset must always be accounted for.
     * A buffer of exactly 7 bytes has 0 bytes available for audio data. */

    uint8_t small_buf[ADTS_HEADER_SIZE];
    uint8_t audio_buf[16];
    memset(small_buf, 0xCC, sizeof(small_buf));
    memset(audio_buf, 0xDD, sizeof(audio_buf));

    /* Zero bytes of audio data should succeed even in minimal buffer */
    int result = safe_adts_copy(small_buf, ADTS_HEADER_SIZE, audio_buf, 0);
    ck_assert_msg(result == 0, "Zero-length copy into header-sized buffer should succeed");

    /* Any non-zero audio data must be rejected */
    result = safe_adts_copy(small_buf, ADTS_HEADER_SIZE, audio_buf, 1);
    ck_assert_msg(result == -1,
        "1-byte audio copy into header-sized buffer must be rejected (no room)");

    /* Buffer smaller than header must always reject */
    result = safe_adts_copy(small_buf, ADTS_HEADER_SIZE - 1, audio_buf, 0);
    ck_assert_msg(result == -1,
        "Buffer smaller than header size must always be rejected");
}
END_TEST

START_TEST(test_null_and_edge_cases)
{
    /* Invariant: Edge case inputs must not cause undefined behavior */
    uint8_t adts_buf[ADTS_BUF_SIZE];
    uint8_t audio_buf[1] = {0xFF};

    memset(adts_buf, 0x00, sizeof(adts_buf));

    /* Exact maximum allowed length */
    size_t max_alen = ADTS_BUF_SIZE - ADTS_HEADER_SIZE;
    uint8_t *large_audio = (uint8_t *)malloc(max_alen);
    ck_assert_msg(large_audio != NULL, "malloc failed");
    memset(large_audio, 0x55, max_alen);

    int result = safe_adts_copy(adts_buf, ADTS_BUF_SIZE, large_audio, max_alen);
    ck_assert_msg(result == 0, "Exact max length must be accepted");

    /* One byte over maximum */
    result = safe_adts_copy(adts_buf, ADTS_BUF_SIZE, large_audio, max_alen + 1);
    ck_assert_msg(result == -1, "One byte over max must be rejected");

    free(large_audio);

    /* Minimum valid case */
    result = safe_adts_copy(adts_buf, ADTS_BUF_SIZE, audio_buf, 1);
    ck_assert_msg(result == 0, "Single byte copy must succeed");
    ck_assert_msg(adts_buf[ADTS_HEADER_SIZE] == 0xFF, "Copied byte must match");
}
END_TEST

Suite *security_suite(void)
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("Security_CWE120_BufferOverflow");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_buffer_read_never_exceeds_declared_length);
    tcase_add_test(tc_core, test_adts_header_offset_boundary);
    tcase_add_test(tc_core, test_null_and_edge_cases);
    suite_add_tcase(s, tc_core);

    return s;
}

int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = security_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
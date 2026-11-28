/*
 * test_ric_json.c -- Unit tests for ric's thingino.json GPIO discovery
 *
 * The loader fills only still-unset (-1) pins, reads the whole file
 * regardless of size (up to its cap), and treats an unusable file as
 * a fault while a missing one is normal. The big-file case is the
 * load-bearing regression: a fixed-size read that truncates the
 * document mid-structure loses every pin even though the "gpio" key
 * itself would have fit.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "greatest.h"
#include "../ric/ric_json.h"

#define FIXTURE "/tmp/ric_json_test.json"

static void write_fixture(const char *data, size_t len)
{
	FILE *f = fopen(FIXTURE, "w");
	if (f) {
		fwrite(data, 1, len, f);
		fclose(f);
	}
}

static ric_config_t fresh_config(void)
{
	ric_config_t c;
	memset(&c, 0, sizeof(c));
	c.gpio_ircut = -1;
	c.gpio_ircut2 = -1;
	c.gpio_irled = -1;
	c.gpio_irled2 = -1;
	c.pwm_irled = -1;
	c.pwm_irled2 = -1;
	c.pwm_irled_level = 100;
	c.pwm_irled2_level = 100;
	c.pulse_ms = 10;
	return c;
}

TEST ric_json_missing_file(void)
{
	ric_config_t c = fresh_config();
	unlink(FIXTURE);
	ric_json_gpio_load(&c, FIXTURE);
	ASSERT_EQ(-1, c.gpio_ircut);
	ASSERT_EQ(-1, c.gpio_irled);
	PASS();
}

TEST ric_json_int_pins(void)
{
	ric_config_t c = fresh_config();
	const char *j = "{\"gpio\":{\"ircut\":57,\"ir850\":8,\"ir940\":9}}";
	write_fixture(j, strlen(j));
	ric_json_gpio_load(&c, FIXTURE);
	ASSERT_EQ(57, c.gpio_ircut);
	ASSERT_EQ(-1, c.gpio_ircut2);
	ASSERT_EQ(8, c.gpio_irled);
	ASSERT_EQ(9, c.gpio_irled2);
	unlink(FIXTURE);
	PASS();
}

TEST ric_json_dual_string_ircut(void)
{
	ric_config_t c = fresh_config();
	const char *j = "{\"gpio\":{\"ircut\":\"52 53\"}}";
	write_fixture(j, strlen(j));
	ric_json_gpio_load(&c, FIXTURE);
	ASSERT_EQ(52, c.gpio_ircut);
	ASSERT_EQ(53, c.gpio_ircut2);
	unlink(FIXTURE);
	PASS();
}

/* {pin, active_low}: the standard's single-inverted-pin form, valid
 * for all three keys; an absent active_low means active-high. */
TEST ric_json_object_pins(void)
{
	ric_config_t c = fresh_config();
	const char *j = "{\"gpio\":{\"ircut\":{\"pin\":57,\"active_low\":true},"
			"\"ir850\":{\"pin\":49,\"active_low\":true},"
			"\"ir940\":{\"pin\":50}}}";
	write_fixture(j, strlen(j));
	ric_json_gpio_load(&c, FIXTURE);
	ASSERT_EQ(57, c.gpio_ircut);
	ASSERT_EQ(-1, c.gpio_ircut2);
	ASSERT(c.ircut_active_low);
	ASSERT_EQ(49, c.gpio_irled);
	ASSERT(c.irled_active_low);
	ASSERT_EQ(50, c.gpio_irled2);
	ASSERT_FALSE(c.irled2_active_low);
	unlink(FIXTURE);
	PASS();
}

TEST ric_json_object_pair_and_timing(void)
{
	ric_config_t c = fresh_config();
	const char *j = "{\"gpio\":{\"ircut\":{\"pin\":[52,53],\"delay_us\":100000}}}";
	write_fixture(j, strlen(j));
	ric_json_gpio_load(&c, FIXTURE);
	ASSERT_EQ(52, c.gpio_ircut);
	ASSERT_EQ(53, c.gpio_ircut2);
	ASSERT_EQ(100, c.pulse_ms);
	unlink(FIXTURE);
	PASS();
}

TEST ric_json_explicit_timing_wins(void)
{
	ric_config_t c = fresh_config();
	c.pulse_ms = 75;
	c.pulse_ms_explicit = true;
	const char *j = "{\"gpio\":{\"ircut\":{\"pin\":[52,53],\"delay_us\":100000}}}";
	write_fixture(j, strlen(j));
	ric_json_gpio_load(&c, FIXTURE);
	ASSERT_EQ(52, c.gpio_ircut);
	ASSERT_EQ(53, c.gpio_ircut2);
	ASSERT_EQ(75, c.pulse_ms);
	unlink(FIXTURE);
	PASS();
}

TEST ric_json_unsupported_drive_mode_rejected(void)
{
	ric_config_t c = fresh_config();
	const char *j = "{\"gpio\":{\"ircut\":{\"pin\":[52,53],\"mode\":\"level-sequence\"}}}";
	write_fixture(j, strlen(j));
	ric_json_gpio_load(&c, FIXTURE);
	ASSERT_EQ(-1, c.gpio_ircut);
	ASSERT_EQ(-1, c.gpio_ircut2);
	unlink(FIXTURE);
	PASS();
}

/* The webui's bookkeeping keys ride in the same object and are not
 * ric's business; a jooan-style {active_on_boot, pin} still reads. */
TEST ric_json_object_extra_keys_ignored(void)
{
	ric_config_t c = fresh_config();
	const char *j = "{\"gpio\":{\"ir850\":{\"active_on_boot\":false,\"pin\":59}}}";
	write_fixture(j, strlen(j));
	ric_json_gpio_load(&c, FIXTURE);
	ASSERT_EQ(59, c.gpio_irled);
	ASSERT_FALSE(c.irled_active_low);
	unlink(FIXTURE);
	PASS();
}

TEST ric_json_object_without_pin_rejected(void)
{
	ric_config_t c = fresh_config();
	const char *j = "{\"gpio\":{\"ircut\":{\"active_low\":true},"
			"\"ir850\":{\"pin\":\"49\"}}}";
	write_fixture(j, strlen(j));
	ric_json_gpio_load(&c, FIXTURE);
	ASSERT_EQ(-1, c.gpio_ircut);
	ASSERT_EQ(-1, c.gpio_irled);
	unlink(FIXTURE);
	PASS();
}

/* The pin's source carries its polarity: a plain form is active-high
 * by definition, so a stale config flag must not invert a discovered
 * pin. */
TEST ric_json_plain_form_resets_polarity(void)
{
	ric_config_t c = fresh_config();
	c.ircut_active_low = true;
	c.irled_active_low = true;
	const char *j = "{\"gpio\":{\"ircut\":\"52 53\",\"ir850\":8}}";
	write_fixture(j, strlen(j));
	ric_json_gpio_load(&c, FIXTURE);
	ASSERT_EQ(52, c.gpio_ircut);
	ASSERT_EQ(53, c.gpio_ircut2);
	ASSERT_FALSE(c.ircut_active_low);
	ASSERT_EQ(8, c.gpio_irled);
	ASSERT_FALSE(c.irled_active_low);
	unlink(FIXTURE);
	PASS();
}

/* 999 is the tmi8152 motor-driver device, not a GPIO: unsupported,
 * never misread as a pin. */
TEST ric_json_tmi8152_unsupported(void)
{
	ric_config_t c = fresh_config();
	const char *j = "{\"gpio\":{\"ircut\":999}}";
	write_fixture(j, strlen(j));
	ric_json_gpio_load(&c, FIXTURE);
	ASSERT_EQ(-1, c.gpio_ircut);
	c = fresh_config();
	const char *j2 = "{\"gpio\":{\"ircut\":\"999\"}}";
	write_fixture(j2, strlen(j2));
	ric_json_gpio_load(&c, FIXTURE);
	ASSERT_EQ(-1, c.gpio_ircut);
	unlink(FIXTURE);
	PASS();
}

/* The retired o/O suffix notation and any other decorated string are
 * rejected whole. The historic failure was strtol stopping at the
 * suffix: ircut=57, ircut2 lost, and a dual-coil filter driven as a
 * held single pin. */
TEST ric_json_suffix_rejected_whole(void)
{
	ric_config_t c = fresh_config();
	const char *j = "{\"gpio\":{\"ircut\":\"57o 58o\"}}";
	write_fixture(j, strlen(j));
	ric_json_gpio_load(&c, FIXTURE);
	ASSERT_EQ(-1, c.gpio_ircut);
	ASSERT_EQ(-1, c.gpio_ircut2);
	unlink(FIXTURE);
	PASS();
}

TEST ric_json_pair_trailing_garbage_rejected(void)
{
	ric_config_t c = fresh_config();
	const char *j = "{\"gpio\":{\"ircut\":\"57 58 59\"}}";
	write_fixture(j, strlen(j));
	ric_json_gpio_load(&c, FIXTURE);
	ASSERT_EQ(-1, c.gpio_ircut);
	ASSERT_EQ(-1, c.gpio_ircut2);
	c = fresh_config();
	const char *j2 = "{\"gpio\":{\"ircut\":\"57 x\"}}";
	write_fixture(j2, strlen(j2));
	ric_json_gpio_load(&c, FIXTURE);
	ASSERT_EQ(-1, c.gpio_ircut);
	unlink(FIXTURE);
	PASS();
}

/* "-1" and "" are the explicit-disable spellings: honored, silent. */
TEST ric_json_string_disable_silent(void)
{
	ric_config_t c = fresh_config();
	const char *j = "{\"gpio\":{\"ircut\":\"-1\",\"ir850\":\"\"}}";
	write_fixture(j, strlen(j));
	ric_json_gpio_load(&c, FIXTURE);
	ASSERT_EQ(-1, c.gpio_ircut);
	ASSERT_EQ(-1, c.gpio_irled);
	unlink(FIXTURE);
	PASS();
}

/* Regression: a device file well past 2 KB with the gpio object at
 * the tail. A bounded read that truncates the document loses every
 * pin; reading the whole file must find them. */
TEST ric_json_big_file_pins_at_tail(void)
{
	ric_config_t c = fresh_config();
	char j[4096];
	char pad[2601];
	memset(pad, 'p', 2600);
	pad[2600] = '\0';
	int len = snprintf(
		j, sizeof(j),
		"{\"pad\":\"%s\",\"gpio\":{\"ircut\":\"52 53\",\"ir850\":47,\"ir940\":49}}", pad);
	ASSERT(len > 2047);
	write_fixture(j, (size_t)len);
	ric_json_gpio_load(&c, FIXTURE);
	ASSERT_EQ(52, c.gpio_ircut);
	ASSERT_EQ(53, c.gpio_ircut2);
	ASSERT_EQ(47, c.gpio_irled);
	ASSERT_EQ(49, c.gpio_irled2);
	unlink(FIXTURE);
	PASS();
}

TEST ric_json_oversize_skipped(void)
{
	ric_config_t c = fresh_config();
	size_t big = 70 * 1024;
	char *j = malloc(big);
	ASSERT(j);
	memset(j, 'x', big);
	memcpy(j, "{\"gpio\":{\"ircut\":57}}", 21);
	write_fixture(j, big);
	free(j);
	ric_json_gpio_load(&c, FIXTURE);
	ASSERT_EQ(-1, c.gpio_ircut);
	unlink(FIXTURE);
	PASS();
}

TEST ric_json_garbage(void)
{
	ric_config_t c = fresh_config();
	const char *j = "not json {{{";
	write_fixture(j, strlen(j));
	ric_json_gpio_load(&c, FIXTURE);
	ASSERT_EQ(-1, c.gpio_ircut);
	ASSERT_EQ(-1, c.gpio_irled);
	unlink(FIXTURE);
	PASS();
}

TEST ric_json_empty_file(void)
{
	ric_config_t c = fresh_config();
	write_fixture("", 0);
	ric_json_gpio_load(&c, FIXTURE);
	ASSERT_EQ(-1, c.gpio_ircut);
	unlink(FIXTURE);
	PASS();
}

TEST ric_json_no_gpio_object(void)
{
	ric_config_t c = fresh_config();
	const char *j = "{\"network\":{\"hostname\":\"cam\"}}";
	write_fixture(j, strlen(j));
	ric_json_gpio_load(&c, FIXTURE);
	ASSERT_EQ(-1, c.gpio_ircut);
	unlink(FIXTURE);
	PASS();
}

TEST ric_json_out_of_range_pins(void)
{
	ric_config_t c = fresh_config();
	const char *j = "{\"gpio\":{\"ircut\":192,\"ir850\":-3}}";
	write_fixture(j, strlen(j));
	ric_json_gpio_load(&c, FIXTURE);
	ASSERT_EQ(-1, c.gpio_ircut);
	ASSERT_EQ(-1, c.gpio_irled);
	unlink(FIXTURE);
	PASS();
}

/* raptor.conf pins are authoritative: only -1 fields are filled */
TEST ric_json_conf_precedence(void)
{
	ric_config_t c = fresh_config();
	c.gpio_ircut = 10;
	const char *j = "{\"gpio\":{\"ircut\":57,\"ir850\":8}}";
	write_fixture(j, strlen(j));
	ric_json_gpio_load(&c, FIXTURE);
	ASSERT_EQ(10, c.gpio_ircut);
	ASSERT_EQ(8, c.gpio_irled);
	unlink(FIXTURE);
	PASS();
}

/*
 * Trailing bytes after the document are the one malformation
 * cJSON_Parse accepts silently; the loader must reject them rather
 * than half-honor the file. Trailing whitespace is not a fault: jct
 * ends every save with a newline.
 */
TEST ric_json_trailing_junk_rejected(void)
{
	ric_config_t c = fresh_config();
	const char *j = "{\"gpio\":{\"ircut\":57}}junk{{{";
	write_fixture(j, strlen(j));
	ric_json_gpio_load(&c, FIXTURE);
	ASSERT_EQ(-1, c.gpio_ircut);
	unlink(FIXTURE);
	PASS();
}

TEST ric_json_trailing_newline_accepted(void)
{
	ric_config_t c = fresh_config();
	const char *j = "{\"gpio\":{\"ircut\":57}}\n";
	write_fixture(j, strlen(j));
	ric_json_gpio_load(&c, FIXTURE);
	ASSERT_EQ(57, c.gpio_ircut);
	unlink(FIXTURE);
	PASS();
}

TEST ric_json_truncated_document(void)
{
	ric_config_t c = fresh_config();
	const char *j = "{\"gpio\":{\"ircut\":5";
	write_fixture(j, strlen(j));
	ric_json_gpio_load(&c, FIXTURE);
	ASSERT_EQ(-1, c.gpio_ircut);
	unlink(FIXTURE);
	PASS();
}

TEST ric_json_all_preset_skips_file(void)
{
	ric_config_t c = fresh_config();
	c.gpio_ircut = 10;
	c.gpio_irled = 11;
	c.gpio_irled2 = 12;
	write_fixture("garbage that must never be read", 31);
	ric_json_gpio_load(&c, FIXTURE);
	ASSERT_EQ(10, c.gpio_ircut);
	ASSERT_EQ(11, c.gpio_irled);
	ASSERT_EQ(12, c.gpio_irled2);
	unlink(FIXTURE);
	PASS();
}

/* A bank on a PWM output: an object with a pwm_channel and no pin.
 * Channel 0 is a channel; an absent pwm_level leaves the level alone;
 * polarity is handed back for the caller to refuse. */
TEST ric_json_pwm_bank(void)
{
	ric_config_t c = fresh_config();
	const char *j = "{\"gpio\":{\"ir850\":{\"pwm_channel\":2,\"pwm_level\":60,"
			"\"active_low\":true,\"active_on_boot\":false},"
			"\"ir940\":{\"pwm_channel\":0}}}";
	write_fixture(j, strlen(j));
	ric_json_gpio_load(&c, FIXTURE);
	ASSERT_EQ(2, c.pwm_irled);
	ASSERT_EQ(60, c.pwm_irled_level);
	ASSERT_EQ(-1, c.gpio_irled);
	ASSERT(c.irled_active_low);
	ASSERT_EQ(0, c.pwm_irled2);
	ASSERT_EQ(100, c.pwm_irled2_level);
	ASSERT_EQ(-1, c.gpio_irled2);
	ASSERT_FALSE(c.irled2_active_low);
	unlink(FIXTURE);
	PASS();
}

/* The load-bearing compatibility case: an object that has a usable pin
 * is the GPIO bank it has always been, whatever PWM keys it carries. */
TEST ric_json_pwm_keys_beside_pin_ignored(void)
{
	ric_config_t c = fresh_config();
	const char *j = "{\"gpio\":{\"ir850\":{\"pin\":49,\"pwm_channel\":2,\"pwm_level\":60},"
			"\"ir940\":{\"pin\":50,\"active_low\":true,\"pwm_level\":30}}}";
	write_fixture(j, strlen(j));
	ric_json_gpio_load(&c, FIXTURE);
	ASSERT_EQ(49, c.gpio_irled);
	ASSERT_EQ(-1, c.pwm_irled);
	ASSERT_EQ(100, c.pwm_irled_level);
	ASSERT_EQ(50, c.gpio_irled2);
	ASSERT(c.irled2_active_low);
	ASSERT_EQ(-1, c.pwm_irled2);
	ASSERT_EQ(100, c.pwm_irled2_level);
	unlink(FIXTURE);
	PASS();
}

/* An unusable pin does not shadow the channel: the pad is not a GPIO. */
TEST ric_json_pwm_bank_with_unusable_pin(void)
{
	ric_config_t c = fresh_config();
	const char *j = "{\"gpio\":{\"ir850\":{\"pin\":-1,\"pwm_channel\":3}}}";
	write_fixture(j, strlen(j));
	ric_json_gpio_load(&c, FIXTURE);
	ASSERT_EQ(3, c.pwm_irled);
	ASSERT_EQ(-1, c.gpio_irled);
	unlink(FIXTURE);
	PASS();
}

TEST ric_json_pwm_bad_channel_rejected(void)
{
	ric_config_t c = fresh_config();
	const char *j = "{\"gpio\":{\"ir850\":{\"pwm_channel\":-1},"
			"\"ir940\":{\"pwm_channel\":\"2\",\"pwm_level\":60}}}";
	write_fixture(j, strlen(j));
	ric_json_gpio_load(&c, FIXTURE);
	ASSERT_EQ(-1, c.pwm_irled);
	ASSERT_EQ(-1, c.gpio_irled);
	ASSERT_EQ(-1, c.pwm_irled2);
	ASSERT_EQ(100, c.pwm_irled2_level);
	unlink(FIXTURE);
	PASS();
}

/* A pwm_level outside 1..100, or not a number, keeps the level in
 * force; the channel is still taken. */
TEST ric_json_pwm_bad_level_keeps_level(void)
{
	ric_config_t c = fresh_config();
	const char *j = "{\"gpio\":{\"ir850\":{\"pwm_channel\":2,\"pwm_level\":0},"
			"\"ir940\":{\"pwm_channel\":3,\"pwm_level\":\"60\"}}}";
	write_fixture(j, strlen(j));
	ric_json_gpio_load(&c, FIXTURE);
	ASSERT_EQ(2, c.pwm_irled);
	ASSERT_EQ(100, c.pwm_irled_level);
	ASSERT_EQ(3, c.pwm_irled2);
	ASSERT_EQ(100, c.pwm_irled2_level);
	c = fresh_config();
	const char *j2 = "{\"gpio\":{\"ir850\":{\"pwm_channel\":2,\"pwm_level\":101}}}";
	write_fixture(j2, strlen(j2));
	ric_json_gpio_load(&c, FIXTURE);
	ASSERT_EQ(2, c.pwm_irled);
	ASSERT_EQ(100, c.pwm_irled_level);
	unlink(FIXTURE);
	PASS();
}

TEST ric_json_pwm_explicit_level_wins(void)
{
	ric_config_t c = fresh_config();
	c.pwm_irled_level = 40;
	c.pwm_irled_level_explicit = true;
	const char *j = "{\"gpio\":{\"ir850\":{\"pwm_channel\":2,\"pwm_level\":60}}}";
	write_fixture(j, strlen(j));
	ric_json_gpio_load(&c, FIXTURE);
	ASSERT_EQ(2, c.pwm_irled);
	ASSERT_EQ(40, c.pwm_irled_level);
	unlink(FIXTURE);
	PASS();
}

/* A bank raptor.conf set either way is not looked up: a configured
 * channel is not joined by a discovered pin, nor a configured pin by a
 * discovered channel. */
TEST ric_json_pwm_conf_precedence(void)
{
	ric_config_t c = fresh_config();
	c.pwm_irled = 5;
	c.gpio_irled2 = 12;
	const char *j = "{\"gpio\":{\"ir850\":8,"
			"\"ir940\":{\"pwm_channel\":3,\"pwm_level\":60}}}";
	write_fixture(j, strlen(j));
	ric_json_gpio_load(&c, FIXTURE);
	ASSERT_EQ(5, c.pwm_irled);
	ASSERT_EQ(-1, c.gpio_irled);
	ASSERT_EQ(12, c.gpio_irled2);
	ASSERT_EQ(-1, c.pwm_irled2);
	ASSERT_EQ(100, c.pwm_irled2_level);
	unlink(FIXTURE);
	PASS();
}

SUITE(ric_json_suite)
{
	RUN_TEST(ric_json_missing_file);
	RUN_TEST(ric_json_int_pins);
	RUN_TEST(ric_json_dual_string_ircut);
	RUN_TEST(ric_json_object_pins);
	RUN_TEST(ric_json_object_pair_and_timing);
	RUN_TEST(ric_json_explicit_timing_wins);
	RUN_TEST(ric_json_unsupported_drive_mode_rejected);
	RUN_TEST(ric_json_object_extra_keys_ignored);
	RUN_TEST(ric_json_object_without_pin_rejected);
	RUN_TEST(ric_json_plain_form_resets_polarity);
	RUN_TEST(ric_json_tmi8152_unsupported);
	RUN_TEST(ric_json_suffix_rejected_whole);
	RUN_TEST(ric_json_pair_trailing_garbage_rejected);
	RUN_TEST(ric_json_string_disable_silent);
	RUN_TEST(ric_json_big_file_pins_at_tail);
	RUN_TEST(ric_json_oversize_skipped);
	RUN_TEST(ric_json_garbage);
	RUN_TEST(ric_json_empty_file);
	RUN_TEST(ric_json_no_gpio_object);
	RUN_TEST(ric_json_out_of_range_pins);
	RUN_TEST(ric_json_trailing_junk_rejected);
	RUN_TEST(ric_json_trailing_newline_accepted);
	RUN_TEST(ric_json_truncated_document);
	RUN_TEST(ric_json_conf_precedence);
	RUN_TEST(ric_json_all_preset_skips_file);
	RUN_TEST(ric_json_pwm_bank);
	RUN_TEST(ric_json_pwm_keys_beside_pin_ignored);
	RUN_TEST(ric_json_pwm_bank_with_unusable_pin);
	RUN_TEST(ric_json_pwm_bad_channel_rejected);
	RUN_TEST(ric_json_pwm_bad_level_keeps_level);
	RUN_TEST(ric_json_pwm_explicit_level_wins);
	RUN_TEST(ric_json_pwm_conf_precedence);
}

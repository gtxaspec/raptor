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

SUITE(ric_json_suite)
{
	RUN_TEST(ric_json_missing_file);
	RUN_TEST(ric_json_int_pins);
	RUN_TEST(ric_json_dual_string_ircut);
	RUN_TEST(ric_json_big_file_pins_at_tail);
	RUN_TEST(ric_json_oversize_skipped);
	RUN_TEST(ric_json_garbage);
	RUN_TEST(ric_json_empty_file);
	RUN_TEST(ric_json_no_gpio_object);
	RUN_TEST(ric_json_out_of_range_pins);
	RUN_TEST(ric_json_conf_precedence);
	RUN_TEST(ric_json_all_preset_skips_file);
}

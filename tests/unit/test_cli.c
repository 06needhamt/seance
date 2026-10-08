/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The commands' option parsing: counts, sizes, rates, and positions given
 * as counts or times.
 */
#include "cmd_util.h"
#include "test.h"

static void test_counts_sizes_rates(void)
{
	unsigned long n = 0;
	LONG w = 0, h = 0;
	DWORD rate = 0, scale = 0;

	CHECK_EQ(cli_parse_count("--frames", "42", &n), 0);
	CHECK_EQ(n, 42);
	CHECK_EQ(cli_parse_count("--frames", "-1", &n), -1);
	CHECK_EQ(cli_parse_count("--frames", "4x", &n), -1);
	CHECK_EQ(cli_parse_count("--frames", "", &n), -1);

	CHECK_EQ(cli_parse_size("--size", "320x240", &w, &h), 0);
	CHECK(w == 320 && h == 240);
	CHECK_EQ(cli_parse_size("--size", "0x240", &w, &h), -1);

	CHECK_EQ(cli_parse_rate("--rate", "30000/1001", &rate, &scale), 0);
	CHECK(rate == 30000 && scale == 1001);
	CHECK_EQ(cli_parse_rate("--rate", "29.97", &rate, &scale), 0);
	CHECK(rate == 30000 && scale == 1001);
	CHECK_EQ(cli_parse_rate("--rate", "12.5", &rate, &scale), 0);
	CHECK(rate == 12500 && scale == 1000);
	CHECK_EQ(cli_parse_rate("--rate", "0", &rate, &scale), -1);
}

static void test_positions(void)
{
	struct cli_position p;

	CHECK_EQ(cli_parse_position("--start", "250", &p), 0);
	CHECK(!p.is_time && p.count == 250);
	CHECK_EQ(cli_position_units(&p, 44100, 1), 250);
	CHECK_EQ(cli_parse_position("--start", "2.5s", &p), 0);
	CHECK(p.is_time && p.seconds == 2.5);
	CHECK_EQ(cli_position_units(&p, 44100, 1), 110250);
	CHECK_EQ(cli_position_units(&p, 30000, 1001), 74);
	CHECK_EQ(cli_parse_position("--start", "1500ms", &p), 0);
	CHECK(p.is_time && p.seconds == 1.5);
	CHECK_EQ(cli_parse_position("--start", "1:02.5", &p), 0);
	CHECK(p.is_time && p.seconds == 62.5);
	CHECK_EQ(cli_parse_position("--start", "1:00:01", &p), 0);
	CHECK(p.is_time && p.seconds == 3601);
	CHECK_EQ(cli_parse_position("--start", ".5s", &p), 0);
	CHECK(p.is_time && p.seconds == 0.5);
	/* 0.1 s at 10 per second is frame 1, not 0.999... */
	CHECK_EQ(cli_parse_position("--start", "0.1s", &p), 0);
	CHECK_EQ(cli_position_units(&p, 10, 1), 1);
	CHECK_EQ(cli_position_units(&p, 0, 1), 0);

	static const char *const bad[] = {
		"", "-1", "-1s", "+2s", "2.5", "2x", "s", "1:", ":5", "1:2:3:4",
		"1::2", "2.5 s", "inf", "nans", "1e400s", "2sm", "2ms5",
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		if (cli_parse_position("--start", bad[i], &p) != -1) {
			fprintf(stderr, "'%s' was taken\n", bad[i]);
			CHECK(0);
		}
	}
}

int main(void)
{
	test_counts_sizes_rates();
	test_positions();
	return test_summary("test_cli");
}

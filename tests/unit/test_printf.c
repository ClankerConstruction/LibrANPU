// SPDX-License-Identifier: GPL-3.0-only

#include <stdio.h>
#include <string.h>
#include "harness.h"

int fw_snprintf(char *buf, size_t size, const char *fmt, ...);

#define SAME(fmt, ...) do {						\
	char a[64], b[64];						\
	fw_snprintf(a, sizeof(a), fmt, __VA_ARGS__);			\
	snprintf(b, sizeof(b), fmt, __VA_ARGS__);			\
	if (strcmp(a, b)) {						\
		fprintf(stderr, "%s: '%s' != '%s'\n", fmt, a, b);	\
		test_failed = 1;					\
	}								\
} while (0)

TEST(conversions)
{
	SAME("%d %d %d", 0, -42, 2147483647);
	SAME("%u %x", 4294967295u, 0xdeadbeefu);
	SAME("%5d|%-5d|%05d|%05d", 42, 42, 42, -42);
	SAME("%08x %s %c %%", 0x1234u, "str", 'z');
}

TEST(truncates)
{
	char a[8];
	int n = fw_snprintf(a, sizeof(a), "%s", "0123456789");

	CHECK_EQ(n, 10);
	CHECK(!strcmp(a, "0123456"));
}

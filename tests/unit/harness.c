// SPDX-License-Identifier: GPL-3.0-only

#include "harness.h"

#define MAX_TESTS	64

static struct test tests[MAX_TESTS];
static int ntests;
int test_failed;

void test_register(const char *name, void (*fn)(void))
{
	tests[ntests].name = name;
	tests[ntests++].fn = fn;
}

int main(void)
{
	int i, failed = 0;

	for (i = 0; i < ntests; i++) {
		test_failed = 0;
		tests[i].fn();
		printf("%s %s\n", test_failed ? "FAIL" : "ok  ", tests[i].name);
		failed += test_failed;
	}
	printf("%d/%d passed\n", ntests - failed, ntests);
	return !!failed;
}

/* SPDX-License-Identifier: GPL-3.0-only */
/* Minimal test harness: TEST() registers, CHECK() counts */
#ifndef __HARNESS_H
#define __HARNESS_H

#include <stdio.h>
#include <stdlib.h>

struct test {
	const char *name;
	void (*fn)(void);
};

extern int test_failed;

#define TEST(n)								\
	static void test_##n(void);					\
	__attribute__((constructor)) static void reg_##n(void)		\
	{ test_register(#n, test_##n); }				\
	static void test_##n(void)

#define CHECK(c) do {							\
	if (!(c)) {							\
		fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);	\
		test_failed = 1;					\
	}								\
} while (0)

#define CHECK_EQ(a, b) do {						\
	long long __a = (long long)(a), __b = (long long)(b);		\
	if (__a != __b) {						\
		fprintf(stderr, "%s:%d: %s == %s: %lld != %lld\n",	\
			__FILE__, __LINE__, #a, #b, __a, __b);		\
		test_failed = 1;					\
	}								\
} while (0)

void test_register(const char *name, void (*fn)(void));

#endif

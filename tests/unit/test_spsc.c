// SPDX-License-Identifier: GPL-3.0-only

#include <pthread.h>
#include "harness.h"
#include "core/spsc.h"

static u8 buf[4096] __attribute__((aligned(32)));

TEST(fill_and_drain)
{
	struct spsc *r = (void *)buf;
	struct spsc_prod p;
	struct spsc_cons c;
	u32 i, v;

	spsc_init(r, 8, 4);
	spsc_prod_init(&p, r);
	spsc_cons_init(&c, r);
	for (i = 0; i < 8; i++)
		CHECK(spsc_push32(&p, i));
	CHECK(!spsc_push32(&p, 99));
	for (i = 0; i < 8; i++)
		CHECK(spsc_pop32(&c, &v) && v == i);
	CHECK(!spsc_pop32(&c, &v));
}

TEST(batch_publish)
{
	struct spsc *r = (void *)buf;
	struct spsc_prod p;
	struct spsc_cons c;
	u32 i;

	spsc_init(r, 16, 8);
	spsc_prod_init(&p, r);
	spsc_cons_init(&c, r);
	CHECK_EQ(spsc_room(&p, 5), 16);
	for (i = 0; i < 5; i++)
		*(u64 *)spsc_slot(&p, i) = 0x100 + i;
	CHECK_EQ(spsc_avail(&c, 1), 0);
	spsc_publish(&p, 5);
	CHECK_EQ(spsc_avail(&c, 5), 5);
	for (i = 0; i < 5; i++)
		CHECK_EQ(*(u64 *)spsc_peek(&c, i), 0x100 + i);
	spsc_release(&c, 5);
	CHECK_EQ(spsc_room(&p, 16), 16);
}

#define N	2000000

static void *producer(void *arg)
{
	struct spsc_prod p;
	u32 i = 0, k, n;

	spsc_prod_init(&p, arg);
	while (i < N) {
		n = spsc_room(&p, 32);
		n = n > 32 ? 32 : n;
		if (n > N - i)
			n = N - i;
		for (k = 0; k < n; k++)
			*(volatile u32 *)spsc_slot(&p, k) = i + k;
		if (n)
			spsc_publish(&p, n);
		i += n;
	}
	return NULL;
}

TEST(two_threads_in_order)
{
	struct spsc *r = (void *)buf;
	struct spsc_cons c;
	pthread_t t;
	u32 next = 0, n, k, bad = 0;

	spsc_init(r, 64, 4);
	spsc_cons_init(&c, r);
	pthread_create(&t, NULL, producer, r);
	while (next < N) {
		n = spsc_avail(&c, 16);
		for (k = 0; k < n; k++)
			bad += *(volatile u32 *)spsc_peek(&c, k) != next + k;
		spsc_release(&c, n);
		next += n;
	}
	pthread_join(t, NULL);
	CHECK_EQ(bad, 0);
	CHECK_EQ(next, N);
}

// SPDX-License-Identifier: GPL-3.0-only

#include <string.h>
#include "harness.h"
#include "core/arena.h"

static u8 mem[4096] __attribute__((aligned(64)));
static struct arena a;

static void setup(void)
{
	memset(mem, 0xee, sizeof(mem));
	arena_init(&a, (uintptr_t)mem, sizeof(mem));
}

TEST(alloc_aligned_zeroed)
{
	u8 *p, *q;

	setup();
	p = arena_alloc(&a, 10, 1, OWNER_CORE);
	q = arena_alloc(&a, 100, 64, OWNER_CORE);
	CHECK(p == mem);
	CHECK(q && !((uintptr_t)q & 63) && q >= p + 10);
	CHECK(q[0] == 0 && q[99] == 0);
	CHECK_EQ(arena_free_bytes(&a), sizeof(mem) - 110);
}

TEST(rejects_bad_requests)
{
	setup();
	CHECK(!arena_alloc(&a, 0, 4, OWNER_CORE));
	CHECK(!arena_alloc(&a, 4, 3, OWNER_CORE));
	CHECK(!arena_alloc(&a, 4, 4, OWNER_FREE));
	CHECK(!arena_alloc(&a, sizeof(mem) + 1, 4, OWNER_CORE));
	CHECK(arena_alloc(&a, sizeof(mem), 4, OWNER_CORE));
	CHECK(!arena_alloc(&a, 1, 1, OWNER_CORE));
}

TEST(free_owner_reuses_gap)
{
	u8 *p, *q, *r;

	setup();
	p = arena_alloc(&a, 1024, 4, OWNER_RADIO0);
	q = arena_alloc(&a, 1024, 4, OWNER_RADIO1);
	arena_free_owner(&a, OWNER_RADIO0);
	r = arena_alloc(&a, 512, 4, OWNER_RADIO1);
	CHECK(r == p);
	CHECK(q == mem + 1024);
	CHECK_EQ(arena_free_bytes(&a), sizeof(mem) - 1536);
	arena_free_owner(&a, OWNER_RADIO1);
	CHECK_EQ(arena_free_bytes(&a), sizeof(mem));
}

TEST(table_full)
{
	int i;

	setup();
	for (i = 0; i < ARENA_BLOCKS; i++)
		CHECK(arena_alloc(&a, 8, 4, OWNER_CORE));
	CHECK(!arena_alloc(&a, 8, 4, OWNER_CORE));
}

TEST(never_overlaps)
{
	u8 *blk[32];
	int i, j;

	setup();
	for (i = 0; i < 32; i++)
		blk[i] = arena_alloc(&a, 16 + i * 8, 1u << (i % 6),
				     OWNER_RADIO0 + (i % 3));
	arena_free_owner(&a, OWNER_RADIO1);
	for (i = 0; i < 32; i += 3)
		CHECK(arena_alloc(&a, 24, 8, OWNER_SVC));
	for (i = 0; i < (int)a.n; i++) {
		CHECK(a.blk[i].start + a.blk[i].size <= (uintptr_t)mem + sizeof(mem));
		for (j = i + 1; j < (int)a.n; j++)
			CHECK(a.blk[i].start + a.blk[i].size <= a.blk[j].start);
	}
	(void)blk;
}

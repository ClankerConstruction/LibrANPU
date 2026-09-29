// SPDX-License-Identifier: GPL-3.0-only

#include <string.h>
#include "harness.h"
#include "core/pool.h"

static u16 stack[64];

TEST(get_put_counts)
{
	struct id_pool p;
	u16 ids[80];
	u32 n, i;

	pool_init(&p, stack, 100, 64);
	n = pool_get(&p, ids, 10);
	CHECK_EQ(n, 10);
	CHECK_EQ(ids[0], 100);
	CHECK_EQ(p.top, 54);
	n = pool_get(&p, ids + 10, 80);
	CHECK_EQ(n, 54);
	CHECK_EQ(p.low, 0);
	CHECK_EQ(pool_get(&p, ids, 1), 0);
	for (i = 0; i < 64; i++)
		pool_put(&p, ids[i], 100, 163);
	CHECK_EQ(p.top, 64);
	CHECK_EQ(p.bad, 0);
}

TEST(every_id_once)
{
	struct id_pool p;
	u16 ids[64];
	u8 seen[64];
	u32 i;

	pool_init(&p, stack, 0, 64);
	CHECK_EQ(pool_get(&p, ids, 64), 64);
	memset(seen, 0, sizeof(seen));
	for (i = 0; i < 64; i++)
		seen[ids[i]]++;
	for (i = 0; i < 64; i++)
		CHECK_EQ(seen[i], 1);
}

TEST(rejects_bad_returns)
{
	struct id_pool p;
	u16 id;

	pool_init(&p, stack, 10, 4);
	pool_put(&p, 11, 10, 13);	/* stack full */
	pool_put(&p, 9, 10, 13);
	CHECK_EQ(pool_get(&p, &id, 1), 1);
	pool_put(&p, 14, 10, 13);
	CHECK_EQ(p.bad, 3);
	pool_put(&p, id, 10, 13);
	CHECK_EQ(p.top, 4);
}

TEST(init_except_held)
{
	u32 held[2] = { BIT(0) | BIT(31), BIT(3) };
	struct id_pool p;
	u16 ids[64];
	u32 i, n;

	CHECK_EQ(pool_init_except(&p, stack, 40, held), 3);
	n = pool_get(&p, ids, 64);
	CHECK_EQ(n, 37);
	CHECK_EQ(ids[0], 1);
	for (i = 0; i < n; i++)
		CHECK(ids[i] != 0 && ids[i] != 31 && ids[i] != 35);
	/* held ids come back later: room for all of them */
	pool_put(&p, 31, 0, 39);
	CHECK_EQ(p.bad, 0);
	CHECK_EQ(pool_init_except(&p, stack, 40, NULL), 0);
	CHECK_EQ(p.top, 40);
}

/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Id pool: a free stack owned by one task. Other harts give ids back
 * through their own SPSC rings, published per batch; only the owner
 * pushes onto the stack.
 */
#ifndef __CORE_POOL_H
#define __CORE_POOL_H

#include "fw/types.h"

struct id_pool {
	u16 *stack;
	u32 top;			/* ids on the stack */
	u32 size;			/* ids the pool was built with */
	u32 low;			/* lowest top seen, for the host */
	u32 bad;			/* returns out of range or on a full stack */
	u32 *map;			/* optional: a bit per free id */
	u32 dup;			/* returns of an id already free */
};

static inline void pool_init(struct id_pool *p, u16 *stack, u32 first,
			     u32 count)
{
	u32 i;

	p->stack = stack;
	p->size = count;
	for (i = 0; i < count; i++)
		stack[i] = first + count - 1 - i;
	p->top = count;
	p->low = count;
	p->bad = 0;
	p->map = NULL;
}

/*
 * ids 0..count-1 but those set in held, a bitmap of 32-bit words read
 * once each; returns how many were left out.
 */
static inline u32 pool_init_except(struct id_pool *p, u16 *stack, u32 count,
				   const volatile u32 *held)
{
	u32 i, bits = 0;

	p->stack = stack;
	p->size = count;
	p->top = 0;
	for (i = count; i-- > 0;) {
		if (held && (i % 32 == 31 || i == count - 1))
			bits = held[i / 32];
		if (!(bits & BIT(i % 32)))
			stack[p->top++] = i;
	}
	p->low = p->top;
	p->bad = 0;
	p->map = NULL;
	return count - p->top;
}

/* map, a bit per id below size: a second return of an id is refused */
static inline void pool_track(struct id_pool *p, u32 *map)
{
	u32 i;

	for (i = 0; i < (p->size + 31) / 32; i++)
		map[i] = 0;
	for (i = 0; i < p->top; i++)
		map[p->stack[i] / 32] |= BIT(p->stack[i] % 32);
	p->map = map;
	p->dup = 0;
}

/* up to n ids into ids[]; returns how many */
static inline u32 pool_get(struct id_pool *p, u16 *ids, u32 n)
{
	u32 i;

	if (n > p->top)
		n = p->top;
	for (i = 0; i < n; i++) {
		ids[i] = p->stack[--p->top];
		if (p->map)
			p->map[ids[i] / 32] &= ~BIT(ids[i] % 32);
	}
	if (p->top < p->low)
		p->low = p->top;
	return n;
}

/* first and last bound the ids this pool hands out */
static inline void pool_put(struct id_pool *p, u16 id, u32 first, u32 last)
{
	if (unlikely(id < first || id > last || p->top == p->size)) {
		p->bad++;
		return;
	}
	if (p->map) {
		if (unlikely(p->map[id / 32] & BIT(id % 32))) {
			p->dup++;
			return;
		}
		p->map[id / 32] |= BIT(id % 32);
	}
	p->stack[p->top++] = id;
}

#endif

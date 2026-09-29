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
	return count - p->top;
}

/* up to n ids into ids[]; returns how many */
static inline u32 pool_get(struct id_pool *p, u16 *ids, u32 n)
{
	u32 i;

	if (n > p->top)
		n = p->top;
	for (i = 0; i < n; i++)
		ids[i] = p->stack[--p->top];
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
	p->stack[p->top++] = id;
}

#endif

/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Single-producer single-consumer ring in uncached SRAM. Each side
 * keeps its own index and a cached copy of the other side's.
 */
#ifndef __CORE_SPSC_H
#define __CORE_SPSC_H

#include "fw/types.h"

struct spsc {
	u32 head;			/* producer writes */
	u32 pad0[7];
	u32 tail;			/* consumer writes */
	u32 pad1[7];
	u32 mask;
	u32 entry_size;
	u32 pad2[6];
	u8 entries[] __aligned(32);
};

struct spsc_prod {
	struct spsc *r;
	u32 head;
	u32 tail;			/* last tail seen */
};

struct spsc_cons {
	struct spsc *r;
	u32 tail;
	u32 head;			/* last head seen */
};

static inline u32 spsc_bytes(u32 entries, u32 entry_size)
{
	return sizeof(struct spsc) + entries * entry_size;
}

static inline void spsc_init(struct spsc *r, u32 entries, u32 entry_size)
{
	r->head = 0;
	r->tail = 0;
	r->mask = entries - 1;
	r->entry_size = entry_size;
}

static inline void spsc_prod_init(struct spsc_prod *p, struct spsc *r)
{
	p->r = r;
	p->head = READ_ONCE(r->head);
	p->tail = READ_ONCE(r->tail);
}

static inline void spsc_cons_init(struct spsc_cons *c, struct spsc *r)
{
	c->r = r;
	c->tail = READ_ONCE(r->tail);
	c->head = READ_ONCE(r->head);
}

static inline void *spsc_entry(struct spsc *r, u32 idx)
{
	return r->entries + (idx & r->mask) * r->entry_size;
}

/* free slots; reloads tail only when the cached view looks short */
static inline u32 spsc_room(struct spsc_prod *p, u32 want)
{
	u32 size = p->r->mask + 1;

	if (size - (p->head - p->tail) < want)
		p->tail = READ_ONCE(p->r->tail);
	return size - (p->head - p->tail);
}

/* slot i of the next batch; write it, then spsc_publish */
static inline void *spsc_slot(struct spsc_prod *p, u32 i)
{
	return spsc_entry(p->r, p->head + i);
}

static inline void spsc_publish(struct spsc_prod *p, u32 n)
{
	p->head += n;
	wmb();
	WRITE_ONCE(p->r->head, p->head);
}

static inline u32 spsc_avail(struct spsc_cons *c, u32 want)
{
	if (c->head - c->tail < want) {
		c->head = READ_ONCE(c->r->head);
		rmb();
	}
	return c->head - c->tail;
}

static inline void *spsc_peek(struct spsc_cons *c, u32 i)
{
	return spsc_entry(c->r, c->tail + i);
}

/* entries read before the producer may reuse their slots */
static inline void spsc_release(struct spsc_cons *c, u32 n)
{
	c->tail += n;
	rwmb();
	WRITE_ONCE(c->r->tail, c->tail);
}

static inline bool spsc_push32(struct spsc_prod *p, u32 v)
{
	if (!spsc_room(p, 1))
		return false;
	*(u32 *)spsc_slot(p, 0) = v;
	spsc_publish(p, 1);
	return true;
}

static inline bool spsc_pop32(struct spsc_cons *c, u32 *v)
{
	if (!spsc_avail(c, 1))
		return false;
	*v = *(volatile u32 *)spsc_peek(c, 0);
	spsc_release(c, 1);
	return true;
}

#endif

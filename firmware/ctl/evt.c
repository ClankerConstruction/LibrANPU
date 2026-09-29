// SPDX-License-Identifier: GPL-3.0-only

#include "fw/csr.h"
#include "fw/errno.h"
#include "fw/lib.h"
#include "ctl/boot.h"
#include "ctl/evt.h"
#include "dbg/dbg.h"
#include "plat/plat.h"

/* at most one host interrupt per 50 us, unless urgent */
#define EVT_IRQ_GAP_US		50

struct evt_ring {
	volatile struct libranpu_evt *ring;
	u32 mask;
	u32 reserve;			/* slots kept for CMD_DONE */
	u32 prod;
	u32 published;
	u32 cons;			/* last host index seen */
	u32 last_irq;
	u32 gap;			/* EVT_IRQ_GAP_US in cycles */
	bool irq_due;
	u32 dropped;
};

static struct evt_ring evt;

void evt_init(void *ring, u32 entries, u32 reserve)
{
	evt.ring = ring;
	evt.mask = entries - 1;
	evt.reserve = reserve;
	evt.prod = evt.published = evt.cons = 0;
	evt.gap = EVT_IRQ_GAP_US * plat_cpu_mhz();
	evt.last_irq = cycles() - evt.gap;
}

static u32 evt_room(void)
{
	u32 size = evt.mask + 1;

	if (size - (evt.prod - evt.cons) <= evt.reserve)
		evt.cons = READ_ONCE(boot->evt_cons);
	/* a host index out of range counts as a full ring */
	if (evt.prod - evt.cons > size)
		return 0;
	return size - (evt.prod - evt.cons);
}

int evt_post(u16 type, u32 seq, const void *payload, u16 len)
{
	volatile struct libranpu_evt *e;
	u32 room = evt_room();

	if (len > LIBRANPU_EVT_PAYLOAD || !evt.ring)
		return -EINVAL;
	if (!room || (type != LIBRANPU_EVT_CMD_DONE && room <= evt.reserve)) {
		evt.dropped++;
		dbg_trace(LIBRANPU_TRACE_EVT_DROP, type, seq);
		return -ENOSPC;
	}

	e = &evt.ring[evt.prod & evt.mask];
	e->type = type;
	e->len = len;
	e->seq = seq;
	if (len)
		copy_words((volatile u32 *)e->payload, payload, len);
	evt.prod++;
	return 0;
}

/*
 * The host writes evt_cons, then reads evt_prod again. If it had not
 * caught up with the last publish it is still draining: no interrupt.
 */
void evt_flush(bool urgent)
{
	u32 now;

	if (!evt.ring)
		return;

	if (evt.prod != evt.published) {
		u32 before = evt.published;

		wmb();
		WRITE_ONCE(boot->evt_prod, evt.prod);
		evt.published = evt.prod;
		mb();
		evt.cons = READ_ONCE(boot->evt_cons);
		if (evt.cons == before)
			evt.irq_due = true;
	}

	if (!evt.irq_due)
		return;
	now = cycles();
	if (!urgent && now - evt.last_irq < evt.gap)
		return;

	evt.irq_due = false;
	evt.last_irq = now;
	plat_notify_host();
}

u32 evt_dropped(void)
{
	return evt.dropped;
}

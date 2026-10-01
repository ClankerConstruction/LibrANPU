// SPDX-License-Identifier: GPL-3.0-only
/*
 * Buffer task: owns the rx buffer pool. It takes ids back from the
 * host return ring and the other tasks, and refills the slots the
 * chip has handed back; on a stop it accounts for every id.
 */

#include "fw/csr.h"
#include "wlan/ppe.h"

#define RET_PROD		8	/* host writes */
#define RET_CONS		0xC	/* we write */
#define REFILL_PUBLISH		64
#define LINE			64

struct buf_ring {
	u32 desc;
	u32 entries;
	u32 regs;
	u32 refill;			/* next slot to refill */
	u32 unpublished;
	u32 count;			/* the radio's buf_refill */
};

struct buf_state {
	u32 epoch;
	struct spsc_cons from_rx;
	struct spsc_cons from_host;
	struct id_pool pool;		/* ours from the reset on */
	u32 ret_cons;
	u32 returned;			/* ids back from the host */
	struct buf_ring rx[WLAN_BANDS];
	/* fixed per attach */
	u32 nbands;
	u32 ids;
	const u8 *ret;			/* host return ring, cached view */
	u32 ret_entries;
	u32 ret_regs;
};

static struct buf_state bs __hart_local;

static void __attribute__((noinline)) buf_reset(struct wlan_radio *r)
{
	u32 b;

	bs.epoch = r->epoch;
	spsc_cons_init(&bs.from_rx, r->rx2buf);
	spsc_cons_init(&bs.from_host, r->host2buf);
	bs.pool = r->pool;
	bs.ret_cons = 0;
	bs.returned = 0;
	bs.nbands = r->nbands;
	bs.ids = r->pool_ids;
	bs.ret = plat_cached((void *)(uintptr_t)r->hret.base);
	bs.ret_entries = r->hret.entries;
	bs.ret_regs = r->hret.regs;
	for (b = 0; b < WLAN_BANDS; b++) {
		bs.rx[b] = (struct buf_ring){
			.desc = r->rx[b].desc,
			.entries = r->rx[b].entries,
			.regs = r->rx[b].regs,
		};
	}
	REG32(bs.ret_regs + RET_CONS) = 0;
}

static inline void put_id(u32 id)
{
	pool_put(&bs.pool, id, 0, bs.ids - 1);
}

static u32 take_spsc(struct spsc_cons *c)
{
	u32 n = spsc_avail(c, 1), i;

	for (i = 0; i < n; i++)
		put_id(*(volatile u32 *)spsc_peek(c, i));
	if (n)
		spsc_release(c, n);
	return n;
}

/*
 * ids the host is done with, read through the cache a line at a time;
 * its index is not trusted past the ring.
 */
static u32 take_host(struct wlan_radio *r)
{
	u32 prod = REG32(bs.ret_regs + RET_PROD), cons = bs.ret_cons, n = 0;

	if (unlikely(prod >= bs.ret_entries))
		prod %= bs.ret_entries;
	while (cons != prod) {
		const volatile u32 *e = (const u32 *)(bs.ret + 4 * cons);
		u32 id;

		/* the host may have added entries to this line since */
		if (!n || !((uintptr_t)e & (LINE - 1)))
			plat_dcache_inv((const void *)((uintptr_t)e & ~(LINE - 1)));
		id = *e & 0xFFFF;
		if (id >= bs.ids)
			r->stats.buf_bad_ret++;
		else
			put_id(id);
		if (++cons == bs.ret_entries)
			cons = 0;
		n++;
	}
	if (n) {
		bs.ret_cons = cons;
		bs.returned += n;
		r->stats.buf_returned = bs.returned;
		REG32(bs.ret_regs + RET_CONS) = cons;
	}
	return n;
}

/* the chip hands slots back in order: refill from w->refill on */
static u32 refill(struct wlan_radio *r, u32 b, u32 budget)
{
	struct buf_ring *w = &bs.rx[b];
	u32 idx = w->refill, n = 0;
	u16 id;

	while (n < budget) {
		volatile u32 *d = (u32 *)(w->desc + 16 * idx);

		if (!(d[1] & WLAN_RX_DESC_DONE))
			break;
		if (!pool_get(&bs.pool, &id, 1)) {
			r->stats.buf_empty++;
			break;
		}
		wlan_rx_slot(r, d, id);
		idx = idx + 1 == w->entries ? 0 : idx + 1;
		n++;
	}
	if (n) {
		w->count += n;
		r->stats.buf_refill[b] = w->count;
		w->refill = idx;
		w->unpublished += n;
		if (w->unpublished >= REFILL_PUBLISH || n < budget) {
			wmb();
			REG32(w->regs + 8) = idx ? idx - 1 : w->entries - 1u;
			w->unpublished = 0;
		}
	}
	return n;
}

/* every id: pool, under a chip descriptor, with the host, or lost */
static void buf_audit(struct wlan_radio *r)
{
	struct libranpu_wlan_audit *a = &r->audit;
	u32 b, i, chip = 0;

	for (b = 0; b < bs.nbands; b++)
		for (i = 0; i < bs.rx[b].entries; i++)
			if (!(REG32(bs.rx[b].desc + 16 * i + 4) &
			      WLAN_RX_DESC_DONE))
				chip++;
	a->free = bs.pool.top;
	a->chip = chip;
	a->host = READ_ONCE(r->delivered) - bs.returned;
	a->transit = spsc_avail(&bs.from_rx, 1) + spsc_avail(&bs.from_host, 1);
	a->fe = ppe_held(r);
	a->lost = bs.ids - a->free - a->chip - a->host - a->transit - a->fe;
}

int wlan_buf_task(struct task *t, int budget)
{
	struct wlan_radio *r = t->ctx;
	u32 st = wlan_state(r), n, b;

	if (st == WLAN_DETACHED)
		return 0;
	if (bs.epoch != r->epoch)
		buf_reset(r);
	if (st != WLAN_RUNNING && st != WLAN_STOPPING) {
		WRITE_ONCE(r->ack[WT_BUF], st);
		return 0;
	}

	n = take_host(r);
	n += take_spsc(&bs.from_rx);
	n += take_spsc(&bs.from_host);
	n += ppe_take(r, &bs.pool, budget * 32, st == WLAN_STOPPING);

	if (st == WLAN_RUNNING) {
		for (b = 0; b < bs.nbands; b++)
			n += refill(r, b, budget * 32);
		return n;
	}

	/* stopping: account once the tasks and the PPE have let go */
	if (READ_ONCE(r->ack[WT_RX]) == WLAN_STOPPING &&
	    READ_ONCE(r->ack[WT_HOST]) == WLAN_STOPPING && !n &&
	    !ppe_held(r)) {
		take_spsc(&bs.from_rx);
		take_spsc(&bs.from_host);
		buf_audit(r);
		wmb();
		WRITE_ONCE(r->ack[WT_BUF], st);
	}
	return n;
}

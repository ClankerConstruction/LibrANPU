// SPDX-License-Identifier: GPL-3.0-only
/*
 * Buffer task: owns the rx buffer pool. It takes ids back from the
 * host return ring and the other tasks, and refills the slots the
 * chip has handed back; on a stop it accounts for every id.
 */

#include "fw/csr.h"
#include "wlan/wlan.h"

#define RET_PROD		8	/* host writes */
#define RET_CONS		0xC	/* we write */
#define REFILL_PUBLISH		64

struct buf_state {
	u32 epoch;
	struct spsc_cons from_rx;
	struct spsc_cons from_host;
	u32 ret_cons;
	u32 refill[WLAN_BANDS];		/* next slot to refill */
	u32 unpublished[WLAN_BANDS];
	u32 returned;			/* ids back from the host */
};

static struct buf_state bs;

static void buf_reset(struct wlan_radio *r)
{
	u32 b;

	bs.epoch = r->epoch;
	spsc_cons_init(&bs.from_rx, r->rx2buf);
	spsc_cons_init(&bs.from_host, r->host2buf);
	bs.ret_cons = 0;
	bs.returned = 0;
	for (b = 0; b < WLAN_BANDS; b++) {
		bs.refill[b] = 0;
		bs.unpublished[b] = 0;
	}
	REG32(r->hret.regs + RET_CONS) = 0;
}

static void put_id(struct wlan_radio *r, u32 id)
{
	pool_put(&r->pool, id, 0, r->pool_ids - 1);
}

static u32 take_spsc(struct wlan_radio *r, struct spsc_cons *c)
{
	u32 n = spsc_avail(c, 1), i;

	for (i = 0; i < n; i++)
		put_id(r, *(volatile u32 *)spsc_peek(c, i));
	if (n)
		spsc_release(c, n);
	return n;
}

/* ids the host is done with; its index is not trusted past the ring */
static u32 take_host(struct wlan_radio *r)
{
	struct wlan_host_ring *h = &r->hret;
	u32 prod = REG32(h->regs + RET_PROD) % h->entries, n = 0;
	volatile u32 *e = (u32 *)h->base;

	while (bs.ret_cons != prod) {
		u32 id = e[bs.ret_cons] & 0xFFFF;

		if (id >= r->pool_ids)
			r->stats.buf_bad_ret++;
		else
			put_id(r, id);
		bs.ret_cons = (bs.ret_cons + 1) % h->entries;
		n++;
	}
	if (n) {
		bs.returned += n;
		r->stats.buf_returned += n;
		REG32(h->regs + RET_CONS) = bs.ret_cons;
	}
	return n;
}

/* the chip hands slots back in order: refill from bs.refill on */
static u32 refill(struct wlan_radio *r, u32 b, u32 budget)
{
	struct wlan_ring *w = &r->rx[b];
	u32 idx = bs.refill[b], n = 0;
	u16 id;

	while (n < budget) {
		volatile u32 *d = (u32 *)(w->desc + 16 * idx);

		if (!(d[1] & WLAN_RX_DESC_DONE))
			break;
		if (!pool_get(&r->pool, &id, 1)) {
			r->stats.buf_empty++;
			break;
		}
		d[0] = wlan_pool_bus(r, id) + LIBRANPU_RX_HEADROOM;
		d[2] = (u32)id << 16;
		d[3] = 0;
		d[1] = WLAN_RX_DESC_CTRL;
		idx = idx + 1 == w->entries ? 0 : idx + 1;
		n++;
	}
	if (n) {
		r->stats.buf_refill[b] += n;
		bs.refill[b] = idx;
		bs.unpublished[b] += n;
		if (bs.unpublished[b] >= REFILL_PUBLISH || n < budget) {
			wmb();
			REG32(w->regs + 8) = idx ? idx - 1 : w->entries - 1u;
			bs.unpublished[b] = 0;
		}
	}
	return n;
}

/* every id: pool, under a chip descriptor, with the host, or lost */
static void buf_audit(struct wlan_radio *r)
{
	struct libranpu_wlan_audit *a = &r->audit;
	u32 b, i, chip = 0;

	for (b = 0; b < r->nbands; b++)
		for (i = 0; i < r->rx[b].entries; i++)
			if (!(REG32(r->rx[b].desc + 16 * i + 4) &
			      WLAN_RX_DESC_DONE))
				chip++;
	a->free = r->pool.top;
	a->chip = chip;
	a->host = READ_ONCE(r->delivered) - bs.returned;
	a->transit = spsc_avail(&bs.from_rx, 1) + spsc_avail(&bs.from_host, 1);
	a->lost = r->pool_ids - a->free - a->chip - a->host - a->transit;
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
	n += take_spsc(r, &bs.from_rx);
	n += take_spsc(r, &bs.from_host);

	if (st == WLAN_RUNNING) {
		for (b = 0; b < r->nbands; b++)
			n += refill(r, b, budget * 32);
		return n;
	}

	/* stopping: account once the rx and host tasks have let go */
	if (READ_ONCE(r->ack[WT_RX]) == WLAN_STOPPING &&
	    READ_ONCE(r->ack[WT_HOST]) == WLAN_STOPPING && !n) {
		take_spsc(r, &bs.from_rx);
		take_spsc(r, &bs.from_host);
		buf_audit(r);
		wmb();
		WRITE_ONCE(r->ack[WT_BUF], st);
	}
	return n;
}

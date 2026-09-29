// SPDX-License-Identifier: GPL-3.0-only
/*
 * Tx task: host tx descriptors into the chip's tx rings. The host ring
 * holds chip descriptors whose TXD and payload stay in host memory; the
 * chip hands slots back in order with the done bit.
 */

#include "fw/csr.h"
#include "wlan/wlan.h"

#define HTX_PROD		8	/* host writes */
#define HTX_CONS		0xC	/* we write */
/* slots kept between us and the chip's descriptor prefetch */
#define TX_RESERVE		8
#define TX_BATCH		64
#define LINE			64

struct tx_state {
	u32 epoch;
	bool started;
	u32 head[WLAN_BANDS];		/* next chip slot we fill */
	u32 tail[WLAN_BANDS];		/* oldest slot the chip holds */
	u32 cons[WLAN_BANDS];		/* next host entry */
};

static struct tx_state ts;

static void tx_reset(struct wlan_radio *r)
{
	u32 b;

	ts.epoch = r->epoch;
	ts.started = false;
	for (b = 0; b < WLAN_BANDS; b++)
		ts.cons[b] = 0;
}

static void tx_begin(struct wlan_radio *r)
{
	u32 b;

	for (b = 0; b < WLAN_BANDS; b++) {
		ts.head[b] = r->tx_start[b];
		ts.tail[b] = r->tx_start[b];
	}
	ts.started = true;
}

static inline u32 ring_next(const struct wlan_ring *w, u32 i)
{
	return i + 1 == w->entries ? 0 : i + 1;
}

/* slots the chip gave back since the last look; SRAM reads only */
static u32 tx_inflight(struct wlan_ring *w, u32 b)
{
	while (ts.tail[b] != ts.head[b] &&
	       (REG32(w->desc + 16 * ts.tail[b] + 4) & WLAN_TX_DESC_DONE))
		ts.tail[b] = ring_next(w, ts.tail[b]);
	return (ts.head[b] + w->entries - ts.tail[b]) % w->entries;
}

/* one descriptor; control word last, again if the chip's late done won */
static void tx_desc(struct wlan_radio *r, u32 d, const volatile u32 *e)
{
	u32 ctrl = e[1] & ~WLAN_TX_DESC_DONE;

	REG32(d) = e[0];
	REG32(d + 8) = e[2];
	REG32(d + 12) = e[3];
	wmb();
	REG32(d + 4) = ctrl;
	if (unlikely(REG32(d + 4) & WLAN_TX_DESC_DONE)) {
		r->stats.tx_rewrite++;
		REG32(d + 4) = ctrl;
	}
}

static u32 tx_band(struct wlan_radio *r, u32 b, u32 budget)
{
	struct wlan_ring *w = &r->tx[b];
	struct wlan_host_ring *h = &r->htx[b];
	const u8 *base = plat_cached((void *)(uintptr_t)h->base);
	u32 prod, avail, room, n;

	prod = REG32(h->regs + HTX_PROD) % h->entries;
	avail = (prod + h->entries - ts.cons[b]) % h->entries;
	if (!avail)
		return 0;
	room = w->entries - 1 - TX_RESERVE - tx_inflight(w, b);
	if ((s32)room <= 0) {
		r->stats.tx_full[b]++;
		return 0;
	}

	n = MIN(MIN(avail, room), budget);
	for (u32 i = 0; i < n; i++) {
		const u8 *e = base + 16 * ts.cons[b];

		/* the host may have added entries to this line since */
		if (!i || !((uintptr_t)e & (LINE - 1)))
			plat_dcache_inv((const void *)((uintptr_t)e & ~(LINE - 1)));
		tx_desc(r, w->desc + 16 * ts.head[b], (const volatile u32 *)e);
		ts.head[b] = ring_next(w, ts.head[b]);
		ts.cons[b] = (ts.cons[b] + 1) % h->entries;
	}

	wmb();
	REG32(w->regs + 8) = ts.head[b];
	REG32(h->regs + HTX_CONS) = ts.cons[b];
	r->stats.tx_descs[b] += n;
	return n;
}

/* stopping: nothing new; done once the chip took every slot */
static bool tx_drained(struct wlan_radio *r)
{
	u32 b, chip = 0, host = 0;

	for (b = 0; b < WLAN_BANDS; b++) {
		struct wlan_host_ring *h = &r->htx[b];

		if (!r->tx[b].desc)
			continue;
		chip += tx_inflight(&r->tx[b], b);
		host += (REG32(h->regs + HTX_PROD) % h->entries + h->entries -
			 ts.cons[b]) % h->entries;
	}
	r->audit.tx_chip = chip;
	r->audit.tx_host = host;
	return !chip;
}

int wlan_tx_task(struct task *t, int budget)
{
	struct wlan_radio *r = t->ctx;
	u32 st = wlan_state(r), n = 0, b;

	if (st == WLAN_DETACHED)
		return 0;
	if (ts.epoch != r->epoch)
		tx_reset(r);
	if ((st != WLAN_RUNNING && st != WLAN_STOPPING) || !r->tx_on) {
		WRITE_ONCE(r->ack[WT_TX], st);
		return 0;
	}
	if (!ts.started)
		tx_begin(r);

	if (st == WLAN_STOPPING) {
		if (tx_drained(r))
			WRITE_ONCE(r->ack[WT_TX], st);
		return 0;
	}

	for (b = 0; b < WLAN_BANDS; b++)
		if (r->tx[b].desc)
			n += tx_band(r, b, (u32)budget * TX_BATCH);
	return n;
}

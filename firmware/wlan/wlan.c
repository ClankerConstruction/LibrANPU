// SPDX-License-Identifier: GPL-3.0-only
/*
 * WLAN service commands: attach places the chip rings in NPU SRAM and
 * opens the PCIe windows; start, stop and detach move the radio state
 * that the datapath tasks follow.
 */

#include "fw/csr.h"
#include "fw/errno.h"
#include "fw/lib.h"
#include "core/arena.h"
#include "wlan/ppe.h"

#define HOST_RX_REGS(n)		NPU_REG(0xD180 + 0x10 * (n))
#define HOST_TX_REGS(n)		NPU_REG(0xD080 + 0x10 * (n))
#define HOST_RINGS		16
#define RING_ALIGN		256
#define STOP_US			50000
#define MOD_FRAMES		32
#define MOD_US			100

struct wlan_radio wlan_radio;

static struct {
	bool stopping;
	struct cmd_ref ref;
	u32 t0;
} wctl;

static u32 chip_bytes(const struct libranpu_wlan_ring *d)
{
	return ALIGN_UP(d->entries * 16u, 64);
}

static int check_ring(const struct libranpu_wlan_attach *a,
		      const struct libranpu_wlan_ring *d)
{
	switch (d->kind) {
	case LIBRANPU_RING_RX_DATA:
	case LIBRANPU_RING_RXDMAD_C:
	case LIBRANPU_RING_TX_DATA:
		if (d->entry_size != 16 || d->entries < 64 ||
		    d->entries > WLAN_RING_MAX || d->link > 1 || !d->regs ||
		    d->band >= a->bands)
			return -EINVAL;
		return 0;
	case LIBRANPU_RING_HOST_RX:
		if (d->entry_size != sizeof(struct libranpu_host_rx) ||
		    d->regs >= HOST_RINGS || d->band >= a->bands ||
		    d->entries < 64 || d->entries > WLAN_RING_MAX)
			return -EINVAL;
		return 0;
	case LIBRANPU_RING_HOST_RET:
		if (d->entry_size != 4 || d->regs >= HOST_RINGS ||
		    d->entries < 64 || d->entries > 2 * a->pool_ids)
			return -EINVAL;
		return 0;
	case LIBRANPU_RING_TXFREE:
		if (d->entry_size != 16 || d->entries < 64 || !d->regs ||
		    d->entries > WLAN_RING_MAX || d->band >= WLAN_BANDS ||
		    d->buf64 < 1 || !plat_host_ptr(d->base, d->entries * 16u))
			return -EINVAL;
		return 0;
	case LIBRANPU_RING_HOST_TXFREE:
		if (d->entry_size != sizeof(struct libranpu_host_txfree) ||
		    d->regs >= HOST_RINGS || d->entries < 64 ||
		    d->entries > WLAN_RING_MAX)
			return -EINVAL;
		return 0;
	case LIBRANPU_RING_HOST_TX:
		if (d->entry_size != 16 || d->regs >= HOST_RINGS ||
		    d->band >= a->bands || d->entries < 64 ||
		    d->entries > WLAN_RING_MAX)
			return -EINVAL;
		return 0;
	case LIBRANPU_RING_NONE:
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static void host_ring(struct wlan_host_ring *h,
		      const struct libranpu_wlan_ring *d, u32 regs)
{
	h->base = (uintptr_t)plat_host_ptr(d->base, d->entries * d->entry_size);
	h->regs = regs;
	h->entries = d->entries;
	h->entry_size = d->entry_size;
}

/*
 * The chip reports tx free into host memory only: the ring stays where
 * the host put it, the NPU just takes and re-arms its slots.
 */
static void txfree_ring(struct wlan_radio *r,
			const struct libranpu_wlan_ring *d)
{
	struct wlan_ring *w = &r->txfree[d->band];

	w->desc = (uintptr_t)plat_host_ptr(d->base, d->entries * 16u);
	w->bus = d->base;
	w->regs = d->regs;
	w->entries = d->entries;
	w->band = d->band;
	r->txfree_arm[d->band] = FIELD_PREP(WLAN_RX_DESC_LEN, d->buf64 * 64u);
}

/* the attach's ring table, read from the host: control only */
static struct libranpu_wlan_ring att_ring[LIBRANPU_WLAN_RINGS];

/*
 * A tx free ring's report must fit the host ring whole, one record a
 * word at most, and every buffer the host armed must be in reach.
 */
static int txfree_check(struct wlan_radio *r, u32 b)
{
	struct wlan_ring *w = &r->txfree[b];
	u32 len = FIELD_GET(WLAN_RX_DESC_LEN, r->txfree_arm[b]), i;

	if (!w->desc)
		return 0;
	if (!r->htxf.base || r->htxf.entries <= len / 2)
		return -EINVAL;
	for (i = 0; i < w->entries; i++) {
		volatile u32 *d = (u32 *)(w->desc + 16 * i);

		/* the host's one empty slot has no buffer */
		if (d[0] && !plat_host_ptr(d[0], len))
			return -ERANGE;
	}
	return 0;
}

/* the rings of one link in one block, and the link's window over it */
static int place_link(struct wlan_radio *r,
		      const struct libranpu_wlan_attach *a, u32 link)
{
	u32 i, total = 0, at;
	u8 *blk;

	for (i = 0; i < a->nrings; i++) {
		const struct libranpu_wlan_ring *d = &att_ring[i];

		if ((d->kind == LIBRANPU_RING_RX_DATA ||
		     d->kind == LIBRANPU_RING_RXDMAD_C ||
		     d->kind == LIBRANPU_RING_TX_DATA) && d->link == link)
			total += chip_bytes(d);
	}
	if (!total)
		return 0;

	blk = arena_alloc(&npu_sram, total, RING_ALIGN, OWNER_RADIO0);
	if (!blk)
		return -ENOSPC;

	for (i = 0, at = (uintptr_t)blk; i < a->nrings; i++) {
		struct libranpu_wlan_ring *d = &att_ring[i];
		struct wlan_ring *w;

		if (d->link != link)
			continue;
		if (d->kind == LIBRANPU_RING_RX_DATA)
			w = &r->rx[d->band];
		else if (d->kind == LIBRANPU_RING_RXDMAD_C)
			w = &r->rxdmad;
		else if (d->kind == LIBRANPU_RING_TX_DATA)
			w = &r->tx[d->band];
		else
			continue;

		w->desc = at;
		w->bus = wlan_bus(at);
		w->regs = d->regs;
		w->entries = d->entries;
		w->band = d->band;
		w->link = link;
		d->base = w->bus;
		at += chip_bytes(d);
	}
	plat_pcie_window(a->link_win[link], wlan_bus((uintptr_t)blk),
			 wlan_bus((uintptr_t)blk + total));
	return 0;
}

/* every rx slot gets a buffer; completions start on a stale generation */
static void fill_rings(struct wlan_radio *r)
{
	u32 b, i;
	u16 id = 0;

	for (b = 0; b < r->nbands; b++) {
		struct wlan_ring *w = &r->rx[b];

		for (i = 0; i < w->entries; i++) {
			volatile u32 *d = (u32 *)(w->desc + 16 * i);

			pool_get(&r->pool, &id, 1);
			wlan_rx_slot(r, d, id);
		}
	}
	for (i = 0; i < r->rxdmad.entries; i++) {
		REG32(r->rxdmad.desc + 16 * i + 4) = 0;
		REG32(r->rxdmad.desc + 16 * i + 8) = WLAN_RXD_NO_ID;
		REG32(r->rxdmad.desc + 16 * i + 12) = WLAN_GEN_STALE;
	}
	/* tx: every slot starts as the chip's, done */
	for (b = 0; b < r->nbands; b++)
		for (i = 0; i < r->tx[b].entries; i++)
			REG32(r->tx[b].desc + 16 * i + 4) = WLAN_TX_DESC_DONE;
}

static int wlan_attach(struct cmd_ctx *c)
{
	const struct libranpu_wlan_attach *a = (const void *)c->req;
	struct wlan_radio *r = &wlan_radio;
	volatile struct libranpu_wlan_ring *table;
	const volatile u32 *held = NULL;
	u32 i, need = 0;
	u16 *stack;
	int err;

	if (a->radio || r->state != WLAN_DETACHED)
		return -EBUSY;
	if (a->backend != LIBRANPU_WLAN_RRO31 || !a->bands ||
	    a->bands > WLAN_BANDS || a->nrings > LIBRANPU_WLAN_RINGS ||
	    a->link_win[0] > 1 || a->link_win[1] > 1 ||
	    !a->pool_ids || a->pool_ids > WLAN_POOL_MAX ||
	    a->pool_base & (LIBRANPU_RX_BUF_SIZE - 1) ||
	    !plat_host_ptr(a->pool_base, a->pool_ids * LIBRANPU_RX_BUF_SIZE) ||
	    a->rx_headroom & 3 || a->rx_buf_len < 256 ||
	    a->rx_headroom + a->rx_buf_len > LIBRANPU_RX_BUF_SIZE)
		return -EINVAL;
	if (a->rx_held) {
		held = plat_host_ptr(a->rx_held, ALIGN_UP(a->pool_ids, 32) / 8);
		if (!held)
			return -EINVAL;
	}
	table = plat_host_ptr(a->ring_table, a->nrings * sizeof(*table));
	if (!table)
		return -EINVAL;
	/* one read of the table; the bases go back at the end */
	for (i = 0; i < a->nrings; i++)
		copy_words((volatile u32 *)&att_ring[i],
			   (const volatile u32 *)&table[i], sizeof(att_ring[i]));

	i = r->epoch;
	memset(r, 0, sizeof(*r));
	r->epoch = i;
	for (i = 0; i < a->nrings; i++) {
		const struct libranpu_wlan_ring *d = &att_ring[i];

		err = check_ring(a, d);
		if (err)
			return err;
		if (d->kind == LIBRANPU_RING_RX_DATA)
			need += d->entries;
		if (d->kind == LIBRANPU_RING_HOST_RX)
			host_ring(&r->hrx[d->band], d, HOST_RX_REGS(d->regs));
		if (d->kind == LIBRANPU_RING_HOST_RET)
			host_ring(&r->hret, d, HOST_TX_REGS(d->regs));
		if (d->kind == LIBRANPU_RING_HOST_TX)
			host_ring(&r->htx[d->band], d, HOST_TX_REGS(d->regs));
		if (d->kind == LIBRANPU_RING_HOST_TXFREE)
			host_ring(&r->htxf, d, HOST_RX_REGS(d->regs));
		if (d->kind == LIBRANPU_RING_TXFREE)
			txfree_ring(r, d);
	}
	if (need >= a->pool_ids || !r->hrx[0].base || !r->hret.base)
		return -EINVAL;
	for (i = 0; i < WLAN_BANDS; i++) {
		err = txfree_check(r, i);
		if (err)
			return err;
	}

	r->nbands = a->bands;
	r->flags = a->flags;
	r->pool_base = a->pool_base;
	r->pool_ids = a->pool_ids;
	r->headroom = a->rx_headroom;
	r->rx_ctrl = FIELD_PREP(WLAN_RX_DESC_LEN, a->rx_buf_len) |
		     WLAN_RX_DESC_TO_HOST;
	r->mod_frames = a->rx_mod_frames ?: MOD_FRAMES;
	r->mod_cycles = (a->rx_mod_us ?: MOD_US) * plat_cpu_mhz();

	stack = arena_alloc(&npu_sram, 2 * a->pool_ids, 4, OWNER_RADIO0);
	r->rx_own = arena_alloc(&npu_sram, a->pool_ids, 4, OWNER_RADIO0);
	r->rx2host = arena_alloc(&npu_sram, spsc_bytes(WLAN_RX2HOST, 8), 32,
				 OWNER_RADIO0);
	r->rx2buf = arena_alloc(&npu_sram, spsc_bytes(WLAN_RETQ, 4), 32,
				OWNER_RADIO0);
	r->host2buf = arena_alloc(&npu_sram, spsc_bytes(WLAN_RETQ, 4), 32,
				  OWNER_RADIO0);
	r->ppe2host = arena_alloc(&npu_sram, spsc_bytes(WLAN_RX2HOST, 8), 32,
				  OWNER_RADIO0);
	err = stack && r->rx_own && r->rx2host && r->rx2buf && r->host2buf &&
	      r->ppe2host ? 0 : -ENOSPC;
	if (!err) {
		for (i = 0; i < a->pool_ids; i++)
			r->rx_own[i] = 0;
		r->held = pool_init_except(&r->pool, stack, a->pool_ids, held);
		/* the pool must outlast the chip's rings */
		if (need >= r->pool.top)
			err = -ENOSPC;
	}
	if (!err)
		err = place_link(r, a, 0);
	if (!err)
		err = place_link(r, a, 1);
	if (!err)
		err = ppe_attach(r);
	if (!err)
		err = lan_attach(r, a->tx_pool_base, a->npu_tokens);
	for (i = 0; i < WLAN_BANDS && !err; i++)
		if (!r->tx[i].desc != !r->htx[i].base ||
		    (r->txfree[i].desc && !r->htxf.base))
			err = -EINVAL;
	if (err || !r->rx[0].desc || !r->rxdmad.desc ||
	    (r->nbands > 1 && !r->rx[1].desc)) {
		arena_free_owner(&npu_sram, OWNER_RADIO0);
		r->state = WLAN_DETACHED;
		return err ?: -EINVAL;
	}

	spsc_init(r->rx2host, WLAN_RX2HOST, 8);
	spsc_init(r->rx2buf, WLAN_RETQ, 4);
	spsc_init(r->host2buf, WLAN_RETQ, 4);
	spsc_init(r->ppe2host, WLAN_RX2HOST, 8);
	fill_rings(r);

	for (i = 0; i < a->nrings; i++)
		table[i].base = att_ring[i].base;
	r->epoch++;
	wmb();
	WRITE_ONCE(r->state, WLAN_ATTACHED);
	return 0;
}

static int wlan_start(struct cmd_ctx *c)
{
	const struct libranpu_wlan_ctl *w = (const void *)c->req;
	struct wlan_radio *r = &wlan_radio;
	u32 b;

	if (w->radio || !(w->dir & LIBRANPU_WLAN_RX))
		return -EINVAL;
	if (r->state != WLAN_ATTACHED)
		return -EBUSY;
	if ((w->dir & LIBRANPU_WLAN_TX) && !r->tx[0].desc)
		return -EOPNOTSUPP;

	/* the host programmed the rings' bases: hand the chip its slots */
	for (b = 0; b < r->nbands; b++)
		REG32(r->rx[b].regs + 8) = r->rx[b].entries - 1;
	REG32(r->rxdmad.regs + 8) = r->rxdmad.entries - 1;
	/* tx starts where the chip is: one bus read per ring, here only */
	r->tx_on = w->dir & LIBRANPU_WLAN_TX;
	for (b = 0; b < WLAN_BANDS && r->tx_on; b++) {
		if (!r->tx[b].desc)
			continue;
		r->tx_start[b] = REG32(r->tx[b].regs + 0xC);
		/* the host did not reset the ring */
		if (r->tx_start[b] >= r->tx[b].entries)
			return -EIO;
		REG32(r->tx[b].regs + 8) = r->tx_start[b];
	}
	if (r->tx_on)
		lan_start(r);
	/* tx free: the host's empty slot is at its cpu index */
	for (b = 0; b < WLAN_BANDS && r->tx_on; b++) {
		if (!r->txfree[b].desc)
			continue;
		r->txfree_start[b] = REG32(r->txfree[b].regs + 8);
		if (r->txfree_start[b] >= r->txfree[b].entries)
			return -EIO;
	}
	wmb();
	WRITE_ONCE(r->state, WLAN_RUNNING);
	return 0;
}

static int wlan_stop(struct cmd_ctx *c)
{
	const struct libranpu_wlan_ctl *w = (const void *)c->req;
	struct wlan_radio *r = &wlan_radio;

	if (w->radio)
		return -EINVAL;
	if (r->state == WLAN_STOPPED || r->state == WLAN_ATTACHED) {
		memcpy(c->rsp, &r->audit, sizeof(r->audit));
		c->rsp_len = sizeof(r->audit);
		return 0;
	}
	if (r->state != WLAN_RUNNING || wctl.stopping)
		return -EBUSY;

	r->no_drain = w->dir & LIBRANPU_WLAN_NO_DRAIN;
	wmb();
	wctl.stopping = true;
	wctl.ref = c->ref;
	wctl.t0 = cycles();
	WRITE_ONCE(r->state, WLAN_STOPPING);
	return CMD_ASYNC;
}

/* control task, every pass: finish a stop once the tasks are drained */
void wlan_ctl_poll(void)
{
	struct wlan_radio *r = &wlan_radio;
	bool done = READ_ONCE(r->ack[WT_BUF]) == WLAN_STOPPING &&
		    (!r->tx_on || READ_ONCE(r->ack[WT_TX]) == WLAN_STOPPING);

	if (!wctl.stopping)
		return;
	if (!done && cycles() - wctl.t0 < STOP_US * plat_cpu_mhz())
		return;

	if (!done)
		r->audit.expired++;
	WRITE_ONCE(r->state, WLAN_STOPPED);
	wctl.stopping = false;
	cmd_finish(&wctl.ref, done ? 0 : -ETIMEDOUT, &r->audit,
		   sizeof(r->audit));
}

static int wlan_detach(struct cmd_ctx *c)
{
	const struct libranpu_wlan_ctl *w = (const void *)c->req;
	struct wlan_radio *r = &wlan_radio;
	u32 l;

	if (w->radio)
		return -EINVAL;
	if (r->state == WLAN_DETACHED)
		return 0;
	if (r->state != WLAN_STOPPED && r->state != WLAN_ATTACHED)
		return -EBUSY;

	memcpy(c->rsp, &r->audit, sizeof(r->audit));
	c->rsp_len = sizeof(r->audit);
	/* a start that failed had no stop: the frame engine goes quiet now */
	lan_stop(r);
	for (l = 0; l < 2; l++)
		plat_pcie_window(l, 0, 0);
	WRITE_ONCE(r->state, WLAN_DETACHED);
	arena_free_owner(&npu_sram, OWNER_RADIO0);
	return 0;
}

static int wlan_force_host(struct cmd_ctx *c)
{
	const struct libranpu_wlan_ctl *w = (const void *)c->req;
	struct wlan_radio *r = &wlan_radio;

	if (w->radio)
		return -EINVAL;
	if (w->on)
		r->flags |= LIBRANPU_WLAN_F_FORCE_HOST;
	else
		r->flags &= ~LIBRANPU_WLAN_F_FORCE_HOST;
	return 0;
}

static int wlan_get_stats(struct cmd_ctx *c)
{
	const struct libranpu_wlan_ctl *w = (const void *)c->req;

	if (w->radio || w->page > 1)
		return -EINVAL;
	if (w->page) {
		struct wlan_lan *l = &wlan_radio.lan;

		/* the tx task owns the pool: a snapshot */
		wlan_radio.txstats.lan_tokens_used = l->tokens ?
			l->tokens - READ_ONCE(l->free.top) : 0;
		memcpy(c->rsp, &wlan_radio.txstats, sizeof(wlan_radio.txstats));
		c->rsp_len = sizeof(wlan_radio.txstats);
		return 0;
	}
	memcpy(c->rsp, &wlan_radio.stats, sizeof(wlan_radio.stats));
	c->rsp_len = sizeof(wlan_radio.stats);
	return 0;
}

/* per-station limit: times go in and out in microseconds */
static int wlan_aqm(struct cmd_ctx *c)
{
	const struct libranpu_wlan_aqm *q = (const void *)c->req;
	struct libranpu_wlan_aqm *o = (void *)c->rsp;
	struct aqm_cfg *a = &wlan_radio.aqm;
	u32 mhz = plat_cpu_mhz();

	if (q->radio)
		return -EINVAL;
	if (q->set) {
		/* 16 intervals must fit the signed 32-bit cycle clock */
		if (!q->interval_us || q->interval_us > LIBRANPU_AQM_INTERVAL_MAX_US ||
		    q->delay_us > q->interval_us)
			return -EINVAL;
		a->limit = q->limit;
		a->target = q->target;
		a->delay = q->delay_us * mhz;
		a->interval = q->interval_us * mhz;
		a->min_q = q->min_q;
		a->small = q->small;
		wmb();
		a->on = q->on;
		wmb();
		wlan_radio.aqm_gen++;
	}
	memset(o, 0, sizeof(*o));
	o->on = a->on;
	o->limit = a->limit;
	o->target = a->target;
	o->delay_us = a->delay / mhz;
	o->interval_us = a->interval / mhz;
	o->min_q = a->min_q;
	o->small = a->small;
	c->rsp_len = sizeof(*o);
	return 0;
}

/* a station's frames in the chip, read racing the tx task: a snapshot */
static int wlan_sta_q(struct cmd_ctx *c)
{
	const struct libranpu_wlan_sta_q *q = (const void *)c->req;
	struct libranpu_wlan_sta_q *o = (void *)c->rsp;
	struct wlan_radio *r = &wlan_radio;
	const struct aqm_sta *s;
	u16 wcid = q->wcid;

	if (q->radio || wcid >= AQM_STAS || !r->lan.sta ||
	    r->state == WLAN_DETACHED)
		return -EINVAL;
	s = &r->lan.sta[wcid];
	memset(o, 0, sizeof(*o));
	o->wcid = wcid;
	o->in_chip = (u16)(s->sent - s->done);
	o->count = s->count;
	o->dropping = aqm_dropping(s);
	o->delay_us = s->delay / plat_cpu_mhz();
	c->rsp_len = sizeof(*o);
	return 0;
}

static const struct cmd_handler wlan_handlers[] = {
	{ LIBRANPU_WLAN_ATTACH, sizeof(struct libranpu_wlan_attach),
	  wlan_attach },
	{ LIBRANPU_WLAN_START, sizeof(struct libranpu_wlan_ctl), wlan_start },
	{ LIBRANPU_WLAN_STOP, sizeof(struct libranpu_wlan_ctl), wlan_stop },
	{ LIBRANPU_WLAN_DETACH, sizeof(struct libranpu_wlan_ctl), wlan_detach },
	{ LIBRANPU_WLAN_FORCE_HOST, sizeof(struct libranpu_wlan_ctl),
	  wlan_force_host },
	{ LIBRANPU_WLAN_GET_STATS, sizeof(struct libranpu_wlan_ctl),
	  wlan_get_stats },
	{ LIBRANPU_WLAN_AQM, sizeof(struct libranpu_wlan_aqm), wlan_aqm },
	{ LIBRANPU_WLAN_STA_Q, sizeof(struct libranpu_wlan_sta_q),
	  wlan_sta_q },
};

const struct cmd_service wlan_service = {
	wlan_handlers, ARRAY_SIZE(wlan_handlers)
};

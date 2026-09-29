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

/* the rings of one link in one block, and the link's window over it */
static int place_link(struct wlan_radio *r,
		      const struct libranpu_wlan_attach *a, u32 link,
		      struct libranpu_wlan_attach_rsp *rsp)
{
	u32 i, total = 0, at;
	u8 *blk;

	for (i = 0; i < a->nrings; i++) {
		const struct libranpu_wlan_ring *d = &a->ring[i];

		if ((d->kind == LIBRANPU_RING_RX_DATA ||
		     d->kind == LIBRANPU_RING_RXDMAD_C) && d->link == link)
			total += chip_bytes(d);
	}
	if (!total)
		return 0;

	blk = arena_alloc(&npu_sram, total, RING_ALIGN, OWNER_RADIO0);
	if (!blk)
		return -ENOSPC;

	for (i = 0, at = (uintptr_t)blk; i < a->nrings; i++) {
		const struct libranpu_wlan_ring *d = &a->ring[i];
		struct wlan_ring *w;

		if (d->link != link)
			continue;
		if (d->kind == LIBRANPU_RING_RX_DATA)
			w = &r->rx[d->band];
		else if (d->kind == LIBRANPU_RING_RXDMAD_C)
			w = &r->rxdmad;
		else
			continue;

		w->desc = at;
		w->bus = wlan_bus(at);
		w->regs = d->regs;
		w->entries = d->entries;
		w->band = d->band;
		w->link = link;
		rsp->ring_base[i] = w->bus;
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
	for (i = 0; i < r->rxdmad.entries; i++)
		REG32(r->rxdmad.desc + 16 * i + 12) = WLAN_GEN_STALE;
}

static int wlan_attach(struct cmd_ctx *c)
{
	const struct libranpu_wlan_attach *a = (const void *)c->req;
	struct libranpu_wlan_attach_rsp *rsp = (void *)c->rsp;
	struct wlan_radio *r = &wlan_radio;
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

	memset(rsp, 0, sizeof(*rsp));
	i = r->epoch;
	memset(r, 0, sizeof(*r));
	r->epoch = i;
	for (i = 0; i < a->nrings; i++) {
		const struct libranpu_wlan_ring *d = &a->ring[i];

		err = check_ring(a, d);
		if (err)
			return err;
		if (d->kind == LIBRANPU_RING_RX_DATA)
			need += d->entries;
		if (d->kind == LIBRANPU_RING_HOST_RX)
			host_ring(&r->hrx[d->band], d, HOST_RX_REGS(d->regs));
		if (d->kind == LIBRANPU_RING_HOST_RET)
			host_ring(&r->hret, d, HOST_TX_REGS(d->regs));
	}
	if (need >= a->pool_ids || !r->hrx[0].base || !r->hret.base)
		return -EINVAL;

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
	r->rx2host = arena_alloc(&npu_sram, spsc_bytes(WLAN_RX2HOST, 8), 32,
				 OWNER_RADIO0);
	r->rx2buf = arena_alloc(&npu_sram, spsc_bytes(WLAN_RETQ, 4), 32,
				OWNER_RADIO0);
	r->host2buf = arena_alloc(&npu_sram, spsc_bytes(WLAN_RETQ, 4), 32,
				  OWNER_RADIO0);
	r->ppe2host = arena_alloc(&npu_sram, spsc_bytes(WLAN_RX2HOST, 8), 32,
				  OWNER_RADIO0);
	err = stack && r->rx2host && r->rx2buf && r->host2buf && r->ppe2host ?
	      0 : -ENOSPC;
	if (!err) {
		r->held = pool_init_except(&r->pool, stack, a->pool_ids, held);
		/* the pool must outlast the chip's rings */
		if (need >= r->pool.top)
			err = -ENOSPC;
	}
	if (!err)
		err = place_link(r, a, 0, rsp);
	if (!err)
		err = place_link(r, a, 1, rsp);
	if (!err)
		err = ppe_attach(r);
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

	r->epoch++;
	wmb();
	WRITE_ONCE(r->state, WLAN_ATTACHED);
	c->rsp_len = sizeof(*rsp);
	return 0;
}

static int wlan_start(struct cmd_ctx *c)
{
	const struct libranpu_wlan_ctl *w = (const void *)c->req;
	struct wlan_radio *r = &wlan_radio;
	u32 b;

	if (w->radio || !(w->dir & LIBRANPU_WLAN_RX))
		return -EINVAL;
	if (w->dir & LIBRANPU_WLAN_TX)
		return -EOPNOTSUPP;
	if (r->state != WLAN_ATTACHED)
		return -EBUSY;

	/* the host programmed the rings' bases: hand the chip its slots */
	for (b = 0; b < r->nbands; b++)
		REG32(r->rx[b].regs + 8) = r->rx[b].entries - 1;
	REG32(r->rxdmad.regs + 8) = r->rxdmad.entries - 1;
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
	bool done = READ_ONCE(r->ack[WT_BUF]) == WLAN_STOPPING;

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

	if (w->radio)
		return -EINVAL;
	memcpy(c->rsp, &wlan_radio.stats, sizeof(wlan_radio.stats));
	c->rsp_len = sizeof(wlan_radio.stats);
	return 0;
}

static const struct cmd_handler wlan_handlers[] = {
	{ LIBRANPU_WLAN_ATTACH, offsetof(struct libranpu_wlan_attach, ring),
	  wlan_attach },
	{ LIBRANPU_WLAN_START, sizeof(struct libranpu_wlan_ctl), wlan_start },
	{ LIBRANPU_WLAN_STOP, sizeof(struct libranpu_wlan_ctl), wlan_stop },
	{ LIBRANPU_WLAN_DETACH, sizeof(struct libranpu_wlan_ctl), wlan_detach },
	{ LIBRANPU_WLAN_FORCE_HOST, sizeof(struct libranpu_wlan_ctl),
	  wlan_force_host },
	{ LIBRANPU_WLAN_GET_STATS, sizeof(struct libranpu_wlan_ctl),
	  wlan_get_stats },
};

const struct cmd_service wlan_service = {
	wlan_handlers, ARRAY_SIZE(wlan_handlers)
};

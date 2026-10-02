// SPDX-License-Identifier: GPL-3.0-only
/*
 * Frame engine setup at attach, and the WiFi buffer FIFO: the buffer
 * task frees the ids of forwarded frames and hands the rest, with
 * their FOE entry and CPU reason, to the host task.
 */

#include "core/arena.h"
#include "fw/errno.h"
#include "fw/lib.h"
#include "wlan/ppe.h"

#define GLO_TX_EN		BIT(0)
#define GLO_BURST		GENMASK(5, 4)
#define GLO_WB_DDONE		BIT(6)
/* required by the tx ring, not documented */
#define GLO_TX_MISC		(BIT(23) | BIT(22) | FIELD_PREP(GENMASK(13, 11), 6))
#define GLO_TX_CLEAR		(GENMASK(23, 20) | GENMASK(13, 11) | GLO_TX_EN)

#define BUF_CFG_EN		BIT(16)
#define BUF_CFG_DROP_UNHIT	BIT(20)	/* unbound: back to us, not a CPU port */
#define BUF_CFG_HW_BUFMNG	BIT(22)
#define BUF_CFG_TICK_1MS	FIELD_PREP(GENMASK(19, 18), 2)
#define BUF_CFG_TIME_THLD	FIELD_PREP(GENMASK(15, 8), 1)
#define BUF_CFG_CLEAR		(BUF_CFG_HW_BUFMNG | GENMASK(19, 18) | \
				 GENMASK(15, 8))

/* buffer task */
static struct {
	u32 epoch;
	struct spsc_prod host;
	volatile u16 *len;
	u32 ids;
	u32 bound;			/* the radio's counters */
	u32 unbound;
	u32 bad;
	u32 dup;
} ps __hart_local;

int ppe_attach(struct wlan_radio *r)
{
	struct wlan_ppe *p = &r->ppe;
	u32 i, g, dtx;

	p->ring = (uintptr_t)arena_alloc(&npu_sram, 8 * PPE_RING, 256,
					 OWNER_RADIO0);
	p->len = arena_alloc(&npu_sram, 2 * r->pool_ids, 4, OWNER_RADIO0);
	p->pool = plat_host_ptr(r->pool_base,
				r->pool_ids * LIBRANPU_RX_BUF_SIZE);
	p->pool_bus = r->pool_base;
	if (!p->ring || !p->len || !p->pool)
		return -ENOSPC;

	/* a non-zero length marks an id at the frame engine */
	for (i = 0; i < r->pool_ids; i++)
		p->len[i] = 0;
	for (i = 0; i < PPE_RING; i++) {
		REG32(p->ring + 8 * i) = TXD_LS;
		REG32(p->ring + 8 * i + 4) = 0;
	}
	REG32(FE_TX_BASE) = wlan_bus(p->ring);
	REG32(FE_TX_CFG) = TX_CFG_8B_13BIT | PPE_RING;
	/* the dma index survives a detach: carry on from it */
	dtx = REG32(FE_TX_DMA_IDX) % PPE_RING;
	REG32(FE_TX_CPU_IDX) = dtx;
	p->idx = dtx;
	p->room = 0;
	p->pending = 0;

	g = REG32(FE_TDMA_GLO_CFG) & ~GLO_TX_CLEAR;
	REG32(FE_TDMA_GLO_CFG) = g | GLO_TX_EN | GLO_BURST | GLO_WB_DDONE |
				 GLO_TX_MISC;
	REG32(FE_TDMA_PPE_FC) |= BIT(0);
	/* hits on bound entries go out; the CPU ones come back to us */
	REG32(FE_WIFI_CRSN_MSK) = BIT(CRSN_HIT_BIND);
	REG32(FE_WIFI_BUF_CFG) = (REG32(FE_WIFI_BUF_CFG) & ~BUF_CFG_CLEAR) |
				 BUF_CFG_EN | BUF_CFG_DROP_UNHIT |
				 BUF_CFG_TICK_1MS | BUF_CFG_TIME_THLD;
	return 0;
}

/* buffer task: one MMIO read per entry; stopping frees every id */
u32 ppe_take(struct wlan_radio *r, struct id_pool *pool, u32 budget,
	     bool stopping)
{
	struct libranpu_wlan_stats *s = &r->stats;
	u32 n = 0, k = 0, bound = ps.bound, unbound = ps.unbound;

	if (ps.epoch != r->epoch) {
		ps.epoch = r->epoch;
		spsc_prod_init(&ps.host, r->ppe2host);
		ps.len = r->ppe.len;
		ps.ids = r->pool_ids;
		bound = unbound = ps.bad = ps.dup = 0;
	}

	while (n < budget) {
		u32 v = REG32(FE_WIFI_BUF_ID), id = FIELD_GET(BUF_ID_ID, v);

		if (!(v & BUF_ID_VALID))
			break;
		if (unlikely(id >= ps.ids)) {
			s->ppe_bad_id = ++ps.bad;
		} else if (unlikely(!ps.len[id])) {
			s->ppe_dup = ++ps.dup;
		} else if ((v & BUF_ID_BOUND) || stopping) {
			ps.len[id] = 0;
			pool_put(pool, id, 0, ps.ids - 1);
			if (v & BUF_ID_BOUND)
				bound++;
			else
				unbound++;
		} else {
			struct wlan_rx_msg *m;
			u32 inf;

			if (spsc_room(&ps.host, k + 1) <= k)
				break;
			inf = REG32(FE_WIFI_PPE_INF);
			m = spsc_slot(&ps.host, k++);
			m->id = id;
			m->len = ps.len[id];
			ps.len[id] = 0;
			m->info = FIELD_PREP(LIBRANPU_HRX_FOE,
					     FIELD_GET(PPE_INF_FOE, inf)) |
				  FIELD_PREP(LIBRANPU_HRX_CRSN,
					     FIELD_GET(PPE_INF_CRSN, inf)) |
				  FIELD_PREP(LIBRANPU_HRX_REASON,
					     LIBRANPU_HRX_PPE) | WRX_LAST;
			unbound++;
			s->ppe_crsn[FIELD_GET(PPE_INF_CRSN, inf)]++;
		}
		REG32(FE_WIFI_BUF_ID) = NPU_FE_POP;
		n++;
	}
	if (k)
		spsc_publish(&ps.host, k);
	if (bound != ps.bound)
		s->ppe_bound = ps.bound = bound;
	if (unbound != ps.unbound)
		s->ppe_unbound = ps.unbound = unbound;
	return n;
}

/* ids the frame engine still holds */
u32 ppe_held(struct wlan_radio *r)
{
	const struct libranpu_wlan_stats *s = &r->stats;

	return READ_ONCE(s->ppe_tx) - s->ppe_bound - s->ppe_unbound -
	       s->ppe_bad_id;
}

// SPDX-License-Identifier: GPL-3.0-only
/*
 * rro31 rx task: walks the chip's RXDMAD_C completion ring, which has
 * no own bit; a 4-bit generation in word 3 marks a fresh descriptor.
 */

#include "fw/csr.h"
#include "wlan/ppe.h"

#define RXD_LEN			GENMASK(29, 16)
#define RXD_LAST		BIT(30)
#define RXD_PN_FAIL		BIT(13)
#define RXD_DST			GENMASK(12, 11)
#define RXD_DST_8023		1
#define RXD_HDR_OFS		GENMASK(6, 0)	/* 2-byte units before 802.3 */
#define RXD_ID			GENMASK(31, 16)
#define RXD_IND		GENMASK(15, 12)	/* indication reason */
#define RXD_IND_REPEAT		1
#define RXD_IND_OLDPKT		2
#define RXD_IND_PN_FAIL		13
#define RXD_TO_HOST		BIT(7)
#define RXD_GEN			GENMASK(31, 28)
#define RX_BATCH		32
#define RX_TO_PPE		0xFF

struct rx_state {
	u32 epoch;
	u32 ridx;
	u32 gen;
	struct spsc_prod host;
	struct spsc_prod drop;
	bool chain;			/* inside a multi-segment frame */
};

static struct rx_state rxs;

static void rx_reset(struct wlan_radio *r)
{
	rxs.epoch = r->epoch;
	rxs.ridx = 0;
	rxs.gen = 0;
	spsc_prod_init(&rxs.host, r->rx2host);
	spsc_prod_init(&rxs.drop, r->rx2buf);
	rxs.chain = false;
}

/* every other indication reason releases a good frame */
static u8 rx_reason(struct wlan_radio *r, u32 w1, u32 w2)
{
	if ((w1 & RXD_PN_FAIL) || FIELD_GET(RXD_IND, w2) == RXD_IND_PN_FAIL) {
		r->stats.rx_pn_fail++;
		return LIBRANPU_HRX_ERROR;
	}
	if (w2 & RXD_TO_HOST)
		return LIBRANPU_HRX_CHIP;
	if (FIELD_GET(RXD_DST, w1) != RXD_DST_8023)
		return LIBRANPU_HRX_RAW;
	if (READ_ONCE(r->flags) & LIBRANPU_WLAN_F_FORCE_HOST)
		return LIBRANPU_HRX_FORCED;
	return RX_TO_PPE;
}

/* the PPE gets the 802.3 frame, the host the whole buffer later */
static bool rx_ppe(struct wlan_radio *r, u32 id, u32 w1)
{
	u32 len = FIELD_GET(RXD_LEN, w1);
	u32 hdr = 2 * FIELD_GET(RXD_HDR_OFS, w1);

	if (!ppe_room(&r->ppe)) {
		r->stats.ppe_full++;
		return false;
	}
	ppe_submit(r, &r->ppe, id, LIBRANPU_RX_HEADROOM + hdr, len - hdr,
		   len);
	r->stats.ppe_tx++;
	return true;
}

/* one burst; false when the next hop is full and the slot must wait */
static bool rx_one(struct wlan_radio *r, u32 w1, u32 w2, bool stopping)
{
	u32 id = FIELD_GET(RXD_ID, w2), ind = FIELD_GET(RXD_IND, w2);
	bool last = w1 & RXD_LAST, chain = rxs.chain || !last;
	struct wlan_rx_msg *m;
	u32 reason;

	if (unlikely(id >= r->pool_ids)) {
		r->stats.rx_bad_id++;
		return true;
	}
	/* the chip's reorder saw these already: never to the stack */
	if (ind == RXD_IND_REPEAT || ind == RXD_IND_OLDPKT) {
		r->stats.rx_stale++;
		stopping = true;
	}
	if (stopping) {
		if (!spsc_room(&rxs.drop, 1))
			return false;
		*(u32 *)spsc_slot(&rxs.drop, 0) = id;
		spsc_publish(&rxs.drop, 1);
		rxs.chain = !last;
		return true;
	}

	reason = rx_reason(r, w1, w2);
	if (reason == RX_TO_PPE && !chain && FIELD_GET(RXD_LEN, w1) >
	    2 * FIELD_GET(RXD_HDR_OFS, w1))
		return rx_ppe(r, id, w1);
	if (reason == RX_TO_PPE)
		reason = LIBRANPU_HRX_CHAIN;

	if (!spsc_room(&rxs.host, 1))
		return false;
	m = spsc_slot(&rxs.host, 0);
	m->id = id;
	m->len = FIELD_GET(RXD_LEN, w1);
	m->info = FIELD_PREP(LIBRANPU_HRX_REASON, reason) |
		  (last ? WRX_LAST : 0);
	spsc_publish(&rxs.host, 1);
	rxs.chain = !last;
	return true;
}

int wlan_rx_task(struct task *t, int budget)
{
	struct wlan_radio *r = t->ctx;
	u32 st = wlan_state(r), n = 0;
	struct wlan_ring *w = &r->rxdmad;

	if (st == WLAN_DETACHED)
		return 0;
	if (rxs.epoch != r->epoch)
		rx_reset(r);
	if (st != WLAN_RUNNING && st != WLAN_STOPPING) {
		WRITE_ONCE(r->ack[WT_RX], st);
		return 0;
	}

	while (n < (u32)budget * RX_BATCH) {
		volatile u32 *d = (u32 *)(w->desc + 16 * rxs.ridx);

		if (FIELD_GET(RXD_GEN, d[3]) != rxs.gen)
			break;
		if (!rx_one(r, d[1], d[2], st == WLAN_STOPPING))
			break;
		r->stats.rx_ind[FIELD_GET(RXD_IND, d[2])]++;
		n++;
		if (++rxs.ridx == w->entries) {
			rxs.ridx = 0;
			rxs.gen = (rxs.gen + 1) & 0xF;
		}
	}

	ppe_kick(&r->ppe);
	if (n) {
		r->stats.rx_frames += n;
		/* the chip may reuse every slot up to the last one read */
		REG32(w->regs + 8) = rxs.ridx ? rxs.ridx - 1 : w->entries - 1u;
	} else if (st == WLAN_STOPPING) {
		WRITE_ONCE(r->ack[WT_RX], st);
	}
	return n;
}

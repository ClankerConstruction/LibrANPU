// SPDX-License-Identifier: GPL-3.0-only
/*
 * rro31 rx task: walks the chip's RXDMAD_C completion ring, which has
 * no own bit; a 4-bit generation in word 3 marks a fresh descriptor.
 */

#include "fw/csr.h"
#include "wlan/wlan.h"

#define RXD_LEN			GENMASK(29, 16)
#define RXD_LAST		BIT(30)
#define RXD_ERR			BIT(13)
#define RXD_DST			GENMASK(12, 11)
#define RXD_ID			GENMASK(31, 16)
#define RXD_ERR_TYPE		GENMASK(15, 12)
#define RXD_TO_HOST		BIT(7)
#define RXD_GEN			GENMASK(31, 28)
#define RX_BATCH		32

struct rx_state {
	u32 epoch;
	u32 ridx;
	u32 gen;
	struct spsc_prod host;
	struct spsc_prod drop;
	u32 bad_id;
	u32 frames;
};

static struct rx_state rxs;

static void rx_reset(struct wlan_radio *r)
{
	rxs.epoch = r->epoch;
	rxs.ridx = 0;
	rxs.gen = 0;
	spsc_prod_init(&rxs.host, r->rx2host);
	spsc_prod_init(&rxs.drop, r->rx2buf);
}

static u8 rx_reason(const struct wlan_radio *r, u32 w1, u32 w2)
{
	if ((w1 & RXD_ERR) || FIELD_GET(RXD_ERR_TYPE, w2))
		return LIBRANPU_HRX_ERROR;
	if (w2 & RXD_TO_HOST)
		return LIBRANPU_HRX_CHIP;
	if (FIELD_GET(RXD_DST, w1) != 1)
		return LIBRANPU_HRX_RAW;
	(void)r;
	return LIBRANPU_HRX_FORCED;
}

/* one burst; false when the next hop is full and the slot must wait */
static bool rx_one(struct wlan_radio *r, u32 w1, u32 w2, bool stopping)
{
	u32 id = FIELD_GET(RXD_ID, w2);
	struct wlan_rx_msg *m;

	if (unlikely(id >= r->pool_ids)) {
		rxs.bad_id++;
		return true;
	}
	if (stopping) {
		if (!spsc_room(&rxs.drop, 1))
			return false;
		*(u32 *)spsc_slot(&rxs.drop, 0) = id;
		spsc_publish(&rxs.drop, 1);
		return true;
	}

	if (!spsc_room(&rxs.host, 1))
		return false;
	m = spsc_slot(&rxs.host, 0);
	m->id = id;
	m->len = FIELD_GET(RXD_LEN, w1);
	m->flags = (w1 & RXD_LAST) ? WRX_LAST : 0;
	m->reason = rx_reason(r, w1, w2);
	m->band = 0;
	spsc_publish(&rxs.host, 1);
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
		n++;
		if (++rxs.ridx == w->entries) {
			rxs.ridx = 0;
			rxs.gen = (rxs.gen + 1) & 0xF;
		}
	}

	if (n) {
		rxs.frames += n;
		/* the chip may reuse every slot up to the last one read */
		REG32(w->regs + 8) = rxs.ridx ? rxs.ridx - 1 : w->entries - 1u;
	} else if (st == WLAN_STOPPING) {
		WRITE_ONCE(r->ack[WT_RX], st);
	}
	return n;
}

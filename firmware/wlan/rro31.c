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
#define RX_PUBLISH		16	/* host task sees frames this often */
#define RX_TO_PPE		0xFF
/* how long the head may wait for the words after its generation */
#define RX_TORN_US		1000

struct rx_state {
	u32 epoch;
	u32 ridx;
	u32 gen;
	bool chain;			/* inside a multi-segment frame */
	bool torn;			/* the head waits for its words */
	u32 torn_t0;
	struct spsc_prod host;
	struct spsc_prod drop;
	struct wlan_ppe ppe;		/* producer side: ours alone */
	/* fixed per attach */
	u32 desc;
	u32 entries;
	u32 regs;
	u32 pool_ids;
	u32 headroom;
	volatile u8 *own;
	u32 torn_cycles;
};

/* one pass: counters go to the radio once, frames to the next hop in batches */
struct rx_pass {
	u32 host;			/* entries not yet published */
	u32 drop;
	u32 ppe;
	bool force;
	bool stopping;
	u32 ind[16];
};

static struct rx_state rxs __hart_local;

static void __attribute__((noinline)) rx_reset(struct wlan_radio *r)
{
	rxs.epoch = r->epoch;
	rxs.ridx = 0;
	rxs.gen = 0;
	spsc_prod_init(&rxs.host, r->rx2host);
	spsc_prod_init(&rxs.drop, r->rx2buf);
	rxs.chain = false;
	rxs.torn = false;
	rxs.own = r->rx_own;
	rxs.torn_cycles = RX_TORN_US * plat_cpu_mhz();
	rxs.ppe = r->ppe;
	rxs.desc = r->rxdmad.desc;
	rxs.entries = r->rxdmad.entries;
	rxs.regs = r->rxdmad.regs;
	rxs.pool_ids = r->pool_ids;
	rxs.headroom = r->headroom;
}

/* every other indication reason releases a good frame */
static u8 rx_reason(struct wlan_radio *r, const struct rx_pass *p, u32 w1,
		    u32 w2)
{
	if ((w1 & RXD_PN_FAIL) || FIELD_GET(RXD_IND, w2) == RXD_IND_PN_FAIL) {
		r->stats.rx_pn_fail++;
		return LIBRANPU_HRX_ERROR;
	}
	if (w2 & RXD_TO_HOST)
		return LIBRANPU_HRX_CHIP;
	if (FIELD_GET(RXD_DST, w1) != RXD_DST_8023)
		return LIBRANPU_HRX_RAW;
	if (p->force)
		return LIBRANPU_HRX_FORCED;
	return RX_TO_PPE;
}

/* the PPE gets the 802.3 frame, the host the whole buffer later */
static bool rx_ppe(struct wlan_radio *r, struct rx_pass *p, u32 id, u32 w1)
{
	u32 len = FIELD_GET(RXD_LEN, w1);
	u32 hdr = 2 * FIELD_GET(RXD_HDR_OFS, w1);

	if (!ppe_room(&rxs.ppe)) {
		r->stats.ppe_full++;
		return false;
	}
	rxs.own[id] = 0;
	ppe_submit(&rxs.ppe, id, rxs.headroom + hdr, len - hdr, len);
	p->ppe++;
	return true;
}

/*
 * The generation word landed before the rest: wait a few passes for it,
 * then take the descriptor as it reads.
 */
static bool rx_torn(struct wlan_radio *r, u32 w1, u32 w2)
{
	if (likely((w2 & RXD_ID) != WLAN_RXD_NO_ID && FIELD_GET(RXD_LEN, w1)))
		return false;
	if (!rxs.torn) {
		rxs.torn = true;
		rxs.torn_t0 = cycles();
		r->stats.rx_torn++;
	}
	return cycles() - rxs.torn_t0 < rxs.torn_cycles;
}

/* an id the chip does not hold: someone else has the buffer */
static bool rx_foreign(struct wlan_radio *r, u32 id, u32 w1)
{
	if (id < rxs.pool_ids)
		r->stats.rx_dup++;
	else
		r->stats.rx_bad_id++;
	rxs.chain = !(w1 & RXD_LAST);
	return true;
}

/* one burst; false when the next hop is full and the slot must wait */
static bool rx_one(struct wlan_radio *r, struct rx_pass *p, u32 w1, u32 w2)
{
	u32 id = FIELD_GET(RXD_ID, w2), ind = FIELD_GET(RXD_IND, w2);
	bool last = w1 & RXD_LAST, chain = rxs.chain || !last;
	bool stopping = p->stopping;
	struct wlan_rx_msg *m;
	u32 reason;

	if (unlikely(id >= rxs.pool_ids || !rxs.own[id]))
		return rx_foreign(r, id, w1);
	/* the chip's reorder saw these already: never to the stack */
	if (ind == RXD_IND_REPEAT || ind == RXD_IND_OLDPKT) {
		r->stats.rx_stale++;
		stopping = true;
	}
	if (stopping) {
		if (spsc_room(&rxs.drop, p->drop + 1) <= p->drop)
			return false;
		rxs.own[id] = 0;
		*(u32 *)spsc_slot(&rxs.drop, p->drop++) = id;
		rxs.chain = !last;
		return true;
	}

	reason = rx_reason(r, p, w1, w2);
	if (reason == RX_TO_PPE && !chain && FIELD_GET(RXD_LEN, w1) >
	    2 * FIELD_GET(RXD_HDR_OFS, w1))
		return rx_ppe(r, p, id, w1);
	if (reason == RX_TO_PPE)
		reason = LIBRANPU_HRX_CHAIN;

	if (spsc_room(&rxs.host, p->host + 1) <= p->host)
		return false;
	rxs.own[id] = 0;
	m = spsc_slot(&rxs.host, p->host++);
	m->id = id;
	m->len = FIELD_GET(RXD_LEN, w1);
	m->info = FIELD_PREP(LIBRANPU_HRX_REASON, reason) |
		  (last ? WRX_LAST : 0);
	rxs.chain = !last;
	if (p->host == RX_PUBLISH) {
		spsc_publish(&rxs.host, p->host);
		p->host = 0;
	}
	return true;
}

static void rx_pass_end(struct wlan_radio *r, struct rx_pass *p, u32 n)
{
	struct libranpu_wlan_stats *s = &r->stats;
	u32 i;

	if (p->host)
		spsc_publish(&rxs.host, p->host);
	if (p->drop)
		spsc_publish(&rxs.drop, p->drop);
	/* counted before the frame engine can hand any of them back */
	if (p->ppe) {
		s->ppe_tx += p->ppe;
		ppe_kick(&rxs.ppe);
	}
	s->rx_frames += n;
	for (i = 0; i < ARRAY_SIZE(p->ind); i++)
		if (p->ind[i])
			s->rx_ind[i] += p->ind[i];
	/* the chip may reuse every slot up to the last one read */
	wmb();
	REG32(rxs.regs + 8) = rxs.ridx ? rxs.ridx - 1 : rxs.entries - 1u;
}

int wlan_rx_task(struct task *t, int budget)
{
	struct wlan_radio *r = t->ctx;
	u32 st = wlan_state(r), n = 0, ridx, gen, max;

	if (st == WLAN_DETACHED)
		return 0;
	if (rxs.epoch != r->epoch)
		rx_reset(r);
	if (st != WLAN_RUNNING && st != WLAN_STOPPING) {
		WRITE_ONCE(r->ack[WT_RX], st);
		return 0;
	}

	ridx = rxs.ridx;
	gen = rxs.gen;
	if (FIELD_GET(RXD_GEN, REG32(rxs.desc + 16 * ridx + 12)) != gen) {
		if (st == WLAN_STOPPING)
			WRITE_ONCE(r->ack[WT_RX], st);
		return 0;
	}

	struct rx_pass p = {
		.force = READ_ONCE(r->flags) & LIBRANPU_WLAN_F_FORCE_HOST,
		.stopping = st == WLAN_STOPPING,
	};

	max = (u32)budget * RX_BATCH;
	do {
		volatile u32 *d = (u32 *)(rxs.desc + 16 * ridx);
		u32 w1, w2;

		if (FIELD_GET(RXD_GEN, d[3]) != gen)
			break;
		w1 = d[1];
		w2 = d[2];
		if (unlikely(rx_torn(r, w1, w2)))
			break;
		if (!rx_one(r, &p, w1, w2))
			break;
		rxs.torn = false;
		/* read: a stale lap no longer looks like a frame */
		d[1] = 0;
		d[2] = WLAN_RXD_NO_ID;
		p.ind[FIELD_GET(RXD_IND, w2)]++;
		n++;
		if (++ridx == rxs.entries) {
			ridx = 0;
			gen = (gen + 1) & 0xF;
		}
	} while (n < max);

	rxs.ridx = ridx;
	rxs.gen = gen;
	if (n)
		rx_pass_end(r, &p, n);
	return n;
}

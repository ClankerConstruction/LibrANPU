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
#define TXF_PROD		8	/* host tx free ring: we write */
#define TXF_CONS		0xC	/* the host writes */

/* chip tx free report */
#define TXF_TYPE		GENMASK(31, 27)
#define TXF_TYPE_NOTIFY		6
#define TXF_MSDUS		GENMASK(25, 16)
#define TXF_VER			GENMASK(19, 16)
#define TXF_PAIR		BIT(31)
#define TXF_HEADER		BIT(30)
#define TXF_WCID		GENMASK(23, 12)
#define TXF_COUNT		GENMASK(27, 24)
#define TXF_STAT		GENMASK(29, 28)
#define TXF_ID			GENMASK(14, 0)
#define TXF_HDR_LEN		8

struct tx_state {
	u32 epoch;
	bool started;
	u32 head[WLAN_BANDS];		/* next chip slot we fill */
	u32 tail[WLAN_BANDS];		/* oldest slot the chip holds */
	u32 cons[WLAN_BANDS];		/* next host entry */
	u32 fidx[WLAN_BANDS];		/* next tx free slot */
	u32 hf_widx;			/* host tx free ring */
	u32 hf_cons;			/* its host index, last read */
	const u8 *pool;			/* rx buffers, cached */
};

static struct tx_state ts;

static void tx_reset(struct wlan_radio *r)
{
	u32 b;

	ts.epoch = r->epoch;
	ts.started = false;
	for (b = 0; b < WLAN_BANDS; b++) {
		ts.cons[b] = 0;
		ts.fidx[b] = 0;
	}
	ts.hf_widx = 0;
	ts.hf_cons = 0;
	ts.pool = plat_cached(plat_host_ptr(r->pool_base, r->pool_ids *
					    LIBRANPU_RX_BUF_SIZE));
	if (r->htxf.base)
		REG32(r->htxf.regs + TXF_PROD) = 0;
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
		r->txstats.rewrite++;
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
		r->txstats.full[b]++;
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
	r->txstats.descs[b] += n;
	return n;
}

static u32 txf_room(struct wlan_radio *r)
{
	struct wlan_host_ring *h = &r->htxf;
	u32 room = (ts.hf_cons + h->entries - ts.hf_widx - 1) % h->entries;

	if (room < h->entries / 2) {
		ts.hf_cons = REG32(h->regs + TXF_CONS) % h->entries;
		room = (ts.hf_cons + h->entries - ts.hf_widx - 1) % h->entries;
	}
	return room;
}

static void txf_put(struct wlan_radio *r, u32 kind, u32 token, u32 wcid,
		    u32 count, u32 failed)
{
	struct wlan_host_ring *h = &r->htxf;
	volatile u32 *e = (u32 *)(h->base + 8 * ts.hf_widx);

	e[0] = token | wcid << 16;
	e[1] = kind | count << 8 | failed << 16;
	ts.hf_widx = ts.hf_widx + 1 == h->entries ? 0 : ts.hf_widx + 1;
}

/*
 * One report: host tokens and station status to the host. Each word
 * gives at most one record, so room for them all is checked first.
 */
static bool txf_report(struct wlan_radio *r, const volatile u32 *ev, u32 len)
{
	u32 words = len / 4, total, seen = 0, wcid = LIBRANPU_TXFREE_NO_WCID;
	u32 ver, i, k;

	if (FIELD_GET(TXF_TYPE, ev[0]) != TXF_TYPE_NOTIFY || len < TXF_HDR_LEN) {
		r->txstats.txfree_bad++;
		return true;
	}
	ver = FIELD_GET(TXF_VER, ev[1]);
	if (ver < 5) {
		r->txstats.txfree_bad++;
		return true;
	}
	if (txf_room(r) < 2 * words)
		return false;

	total = FIELD_GET(TXF_MSDUS, ev[0]);
	for (i = 2; i < words && seen < total; i++) {
		u32 v = ev[i];

		if (v & TXF_PAIR) {
			wcid = FIELD_GET(TXF_WCID, v);
			/* version 7 pairs take two words */
			if (ver == 7 && i + 1 < words && (ev[i + 1] & TXF_PAIR))
				i++;
			continue;
		}
		if (v & TXF_HEADER) {
			if (wcid != LIBRANPU_TXFREE_NO_WCID)
				txf_put(r, LIBRANPU_TXFREE_STATUS, 0, wcid,
					FIELD_GET(TXF_COUNT, v),
					!!FIELD_GET(TXF_STAT, v));
			continue;
		}
		for (k = 0; k < 2; k++) {
			u32 id = (v >> (15 * k)) & TXF_ID;

			if (id == TXF_ID)
				continue;
			seen++;
			txf_put(r, LIBRANPU_TXFREE_TOKEN, id, wcid, 0, 0);
			r->txstats.txfree_host++;
		}
	}
	return true;
}

/* the chip's tx free reports of one ring; slots keep their buffers */
static u32 txf_ring(struct wlan_radio *r, u32 b, u32 budget)
{
	struct wlan_ring *w = &r->txfree[b];
	u32 n = 0, idx = ts.fidx[b];

	while (n < budget) {
		volatile u32 *d = (u32 *)(w->desc + 16 * idx);
		u32 ctrl = d[1], len = FIELD_GET(WLAN_RX_DESC_LEN, ctrl);
		u32 off = d[0] - r->pool_base, o;

		if (!(ctrl & WLAN_TX_DESC_DONE))
			break;
		if (!(ctrl & WLAN_RX_DESC_LAST) || len > LIBRANPU_RX_BUF_SIZE ||
		    off >= r->pool_ids * LIBRANPU_RX_BUF_SIZE) {
			r->txstats.txfree_bad++;
		} else {
			const u8 *ev = ts.pool + off;

			for (o = 0; o < len; o += LINE)
				plat_dcache_inv(ev + o);
			if (!txf_report(r, (const volatile u32 *)ev, len)) {
				r->txstats.txfree_full++;
				break;
			}
			r->txstats.txfree_events[b]++;
		}
		d[1] = FIELD_PREP(WLAN_RX_DESC_LEN, LIBRANPU_RX_BUF_SIZE);
		idx = idx + 1 == w->entries ? 0 : idx + 1;
		n++;
	}
	if (n) {
		ts.fidx[b] = idx;
		wmb();
		REG32(r->htxf.regs + TXF_PROD) = ts.hf_widx;
		REG32(w->regs + 8) = idx ? idx - 1 : w->entries - 1u;
	}
	return n;
}

static u32 txf_all(struct wlan_radio *r, u32 budget)
{
	u32 b, n = 0;

	for (b = 0; b < WLAN_BANDS; b++)
		if (r->txfree[b].desc)
			n += txf_ring(r, b, budget);
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

	/* reports of frames already in the chip still reach the host */
	n = txf_all(r, (u32)budget * 8);
	if (st == WLAN_STOPPING) {
		if (tx_drained(r))
			WRITE_ONCE(r->ack[WT_TX], st);
		return n;
	}

	for (b = 0; b < WLAN_BANDS; b++)
		if (r->tx[b].desc)
			n += tx_band(r, b, (u32)budget * TX_BATCH);
	return n;
}

// SPDX-License-Identifier: GPL-3.0-only
/*
 * LAN to WiFi. The frame engine writes frames of flows bound to WiFi
 * into NPU token buffers on its TDMA rx rings; the tx task puts a TXP
 * in each buffer's headroom and hands the frame to its band's chip
 * ring. The token comes back with the chip's tx free report.
 */

#include "core/arena.h"
#include "fw/errno.h"
#include "wlan/ppe.h"

#define TDMA_RX(k)		FE_REG(0x900 + 0x10 * (k))
#define TDMA_RX_CFG		FE_REG(0x4710)
#define TDMA_FC_CFG0		FE_REG(0x21F0)
#define TDMA_FC_CFG1		FE_REG(0x21F4)
#define TDMA_RX_CFG_EN		BIT(31)
#define TDMA_FC_EN		0x40004000
#define TDMA_PPE_FC_RX		GENMASK(1, 0)
#define GLO_RX_EN		BIT(2)
/* set with the rx ring, not documented */
#define GLO_RX_MISC		FIELD_PREP(GENMASK(18, 16), 4)
#define GLO_RX_CLEAR		(GENMASK(19, 16) | GLO_RX_EN)
#define RING_CNT_KEEP		0x8000F000

#define LAN_DESC		32
#define LAN_HDR			128	/* TXD and TXP before the frame */
#define LAN_BUF_LEN		(LIBRANPU_RX_BUF_SIZE - LAN_HDR)
#define RXD_DONE		BIT(31)
#define RXD_LEN			GENMASK(15, 0)
#define RXD_BAND		BIT(25)		/* word 4 */
#define RXD_WCID		GENMASK(24, 14)	/* word 4 */
#define RXD_BSS			GENMASK(30, 24)	/* word 6 */

/* TXP after an all-zero TXD: the chip builds the TXD from it */
#define TXP_FROM_HOST		0x80
#define TXP_TOKEN		GENMASK(31, 16)
#define TXP_WCID		GENMASK(19, 8)
#define TXP_NBUF_1		BIT(24)
#define LAN_TX_CTRL		0x004C4048	/* 76-byte TXD + TXP, 72-byte head */

/* tx task: the attach's LAN state, its token pool and counters */
static struct {
	struct wlan_lan l;
	u32 ridx[WLAN_LAN_RINGS];
	struct aqm_cfg aqm;
	u32 aqm_gen;
	u32 frames[WLAN_LAN_RINGS];
} ls __hart_local;

static u32 tok_bus(const struct wlan_lan *l, u32 t)
{
	return l->pool_bus + t * LIBRANPU_RX_BUF_SIZE;
}

static volatile u32 *tok_buf(const struct wlan_lan *l, u32 t)
{
	return (u32 *)(l->pool + t * LIBRANPU_RX_BUF_SIZE);
}

/* a TDMA rx slot gets token t, control word last */
static void slot_arm(struct wlan_lan *l, u32 k, u32 i, u32 t)
{
	volatile u32 *d = (u32 *)(l->desc[k] + LAN_DESC * i);

	l->slot_tok[k][i] = t;
	d[2] = tok_bus(l, t) + LAN_HDR;
	wmb();
	d[1] = LAN_BUF_LEN;
}

int lan_attach(struct wlan_radio *r, u32 base, u32 tokens)
{
	struct wlan_lan *l = &r->lan;
	u32 k, i, w;
	u16 *stack, t;
	void *pool;

	if (!tokens)
		return 0;
	pool = plat_host_ptr(base, tokens * LIBRANPU_RX_BUF_SIZE);
	if (!pool || base & (LIBRANPU_RX_BUF_SIZE - 1) || !r->tx[0].desc ||
	    tokens <= WLAN_LAN_RINGS * WLAN_LAN_RING || tokens > WLAN_TOKENS_MAX)
		return -EINVAL;

	stack = arena_alloc(&npu_sram, 2 * tokens, 4, OWNER_RADIO0);
	l->tok_sta = arena_alloc(&npu_sram, 2 * tokens, 4, OWNER_RADIO0);
	l->sta = arena_alloc(&npu_sram, AQM_STAS * sizeof(*l->sta), 4,
			     OWNER_RADIO0);
	for (k = 0; k < WLAN_LAN_RINGS; k++) {
		l->desc[k] = (uintptr_t)arena_alloc(&npu_sram,
						    LAN_DESC * WLAN_LAN_RING,
						    256, OWNER_RADIO0);
		l->slot_tok[k] = arena_alloc(&npu_sram, 2 * WLAN_LAN_RING, 4,
					     OWNER_RADIO0);
		if (!l->desc[k] || !l->slot_tok[k])
			return -ENOSPC;
	}
	if (!stack || !l->tok_sta || !l->sta)
		return -ENOSPC;
	for (i = 0; i < AQM_STAS; i++)
		aqm_sta_init(&l->sta[i]);
	for (t = 0; t < tokens; t++)
		l->tok_sta[t] = AQM_NOT_SENT;
	aqm_defaults(&r->aqm, plat_cpu_mhz());

	l->pool = (uintptr_t)pool;
	l->pool_bus = base;
	l->tokens = tokens;
	/* the TXD and the TXP words never written per frame stay zero */
	for (t = 0; t < tokens; t++)
		for (w = 0; w < LAN_HDR / 4; w++)
			tok_buf(l, t)[w] = 0;

	pool_init(&l->free, stack, 0, tokens);
	REG32(FE_TDMA_GLO_CFG) &= ~BIT(19);
	for (k = 0; k < WLAN_LAN_RINGS; k++) {
		for (i = 0; i < WLAN_LAN_RING; i++) {
			volatile u32 *d = (u32 *)(l->desc[k] + LAN_DESC * i);

			for (w = 0; w < LAN_DESC / 4; w++)
				d[w] = 0;
			pool_get(&l->free, &t, 1);
			slot_arm(l, k, i, t);
		}
		REG32(TDMA_RX(k)) = wlan_bus(l->desc[k]);
		REG32(TDMA_RX(k) + 4) = (REG32(TDMA_RX(k) + 4) & RING_CNT_KEEP) |
					WLAN_LAN_RING;
		/* the dma index survives a detach: carry on from it */
		l->start[k] = REG32(TDMA_RX(k) + 0xC) % WLAN_LAN_RING;
		REG32(TDMA_RX(k) + 8) = l->start[k] ? l->start[k] - 1 :
				       WLAN_LAN_RING - 1;
	}
	REG32(TDMA_RX_CFG) |= TDMA_RX_CFG_EN;
	REG32(TDMA_FC_CFG0) |= TDMA_FC_EN;
	REG32(TDMA_FC_CFG1) |= TDMA_FC_EN;
	REG32(FE_TDMA_PPE_FC) |= TDMA_PPE_FC_RX;
	return 0;
}

void lan_start(struct wlan_radio *r)
{
	u32 g;

	if (!r->lan.tokens)
		return;
	g = REG32(FE_TDMA_GLO_CFG) & ~GLO_RX_CLEAR;
	REG32(FE_TDMA_GLO_CFG) = g | GLO_RX_EN | GLO_RX_MISC;
}

void lan_reset(struct wlan_radio *r)
{
	u32 k;

	ls.l = r->lan;
	for (k = 0; k < WLAN_LAN_RINGS; k++) {
		ls.ridx[k] = ls.l.start[k];
		ls.frames[k] = 0;
	}
	ls.aqm_gen = READ_ONCE(r->aqm_gen);
	rmb();
	ls.aqm = r->aqm;
}

/* counters and the free token count, once per tx pass */
void lan_publish(struct wlan_radio *r)
{
	u32 k;

	if (!ls.l.tokens)
		return;
	for (k = 0; k < WLAN_LAN_RINGS; k++)
		r->txstats.lan_frames[k] = ls.frames[k];
	r->lan.free.top = ls.l.free.top;
}

/* no more frames from the frame engine: its rings go with the detach */
void lan_stop(struct wlan_radio *r)
{
	if (r->lan.tokens && (REG32(FE_TDMA_GLO_CFG) & GLO_RX_EN))
		REG32(FE_TDMA_GLO_CFG) &= ~GLO_RX_EN;
}

/* a report for a token the chip does not hold would free it twice */
bool lan_token_free(struct wlan_radio *r, u32 id)
{
	struct wlan_lan *l = &ls.l;
	u16 w;

	if (id >= l->tokens)
		return false;
	w = l->tok_sta[id];
	if (w == AQM_NOT_SENT) {
		r->txstats.txfree_stale++;
		return true;
	}
	l->tok_sta[id] = AQM_NOT_SENT;
	if (w != AQM_NONE)
		aqm_done(&l->sta[w], id);
	pool_put(&l->free, id, 0, l->tokens - 1);
	return true;
}

/* the chip's time in it, in ms, for a group of a station's frames */
void lan_delay(struct wlan_radio *r, u32 wcid, u32 ms)
{
	if (ls.l.tokens && wcid < AQM_STAS)
		aqm_report(&ls.aqm, &ls.l.sta[wcid], ms);
}

u32 lan_in_chip(struct wlan_radio *r)
{
	struct wlan_lan *l = &ls.l;

	if (!l->tokens)
		return 0;
	return l->tokens - l->free.top - WLAN_LAN_RINGS * WLAN_LAN_RING;
}

/*
 * One frame to the chip: the slot takes a fresh token first, so a
 * frame waits in its slot while the chip ring is full or no token is
 * free; the frame engine backs off.
 */
static bool lan_frame(struct wlan_radio *r, u32 k, u32 i, u32 now)
{
	struct wlan_lan *l = &ls.l;
	volatile u32 *d = (u32 *)(l->desc[k] + LAN_DESC * i);
	u32 len = FIELD_GET(RXD_LEN, d[1]), w4 = d[4], w6 = d[6];
	u32 b = FIELD_GET(RXD_BAND, w4), t = l->slot_tok[k][i], bus;
	u32 wcid = FIELD_GET(RXD_WCID, w4);
	struct aqm_sta *s = wcid < AQM_STAS ? &l->sta[wcid] : NULL;
	enum aqm_verdict v = AQM_PASS;
	volatile u32 *txp;
	u16 next;

	if (!len || len > LAN_BUF_LEN || !wlan_tx_ring(b)) {
		r->txstats.lan_bad++;
		slot_arm(l, k, i, t);
		return true;
	}
	if (s)
		v = aqm_decide(&ls.aqm, s, len, now);
	if (v != AQM_PASS) {
		if (v == AQM_LIMIT)
			r->txstats.lan_limit_drops++;
		else
			r->txstats.lan_aqm_drops++;
		/* the frame goes, its token stays under the slot */
		slot_arm(l, k, i, t);
		return true;
	}
	if (!wlan_tx_room(r, b)) {
		r->txstats.lan_ring_full++;
		return false;
	}
	if (!pool_get(&l->free, &next, 1)) {
		r->txstats.lan_no_token++;
		return false;
	}
	slot_arm(l, k, i, next);

	bus = tok_bus(l, t);
	txp = tok_buf(l, t) + 8;
	txp[0] = TXP_FROM_HOST | FIELD_PREP(TXP_TOKEN, t);
	txp[1] = FIELD_GET(RXD_BSS, w6) |
		 FIELD_PREP(TXP_WCID, FIELD_GET(RXD_WCID, w4)) | TXP_NBUF_1;
	txp[2] = bus + LAN_HDR;
	txp[8] = len;
	wlan_tx_put(r, b, bus, LAN_TX_CTRL, bus + LAN_HDR, 0);
	l->tok_sta[t] = s ? wcid : AQM_NONE;
	if (s)
		aqm_sent(s, t, now);
	ls.frames[k]++;
	return true;
}

u32 lan_drain(struct wlan_radio *r, u32 budget)
{
	struct wlan_lan *l = &ls.l;
	u32 k, n, all = 0, now = cycles(), gen;

	if (!l->tokens)
		return 0;
	/* control changed the limit: take the new one whole */
	gen = READ_ONCE(r->aqm_gen);
	if (gen != ls.aqm_gen) {
		rmb();
		ls.aqm = r->aqm;
		ls.aqm_gen = gen;
	}
	for (k = 0; k < WLAN_LAN_RINGS; k++) {
		u32 i = ls.ridx[k];

		for (n = 0; n < budget; n++) {
			volatile u32 *d = (u32 *)(l->desc[k] + LAN_DESC * i);

			if (!(d[1] & RXD_DONE) || !lan_frame(r, k, i, now))
				break;
			i = i + 1 == WLAN_LAN_RING ? 0 : i + 1;
		}
		if (!n)
			continue;
		ls.ridx[k] = i;
		wmb();
		/* the frame engine may fill up to the last slot we took */
		REG32(TDMA_RX(k) + 8) = i ? i - 1 : WLAN_LAN_RING - 1;
		all += n;
	}
	return all;
}

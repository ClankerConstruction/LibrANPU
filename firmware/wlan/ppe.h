/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Frame engine path: the rx task hands whole 802.3 frames to the PPE
 * on a TDMA tx ring; every id comes back on the WiFi buffer FIFO.
 */
#ifndef __WLAN_PPE_H
#define __WLAN_PPE_H

#include "fw/csr.h"
#include "wlan/wlan.h"

#define FE_REG(off)		(NPU_FE_BASE + (off))
#define FE_TX_BASE		FE_REG(0x800)	/* TDMA tx ring 0 */
#define FE_TX_CFG		FE_REG(0x804)
#define FE_TX_CPU_IDX		FE_REG(0x808)
#define FE_TX_DMA_IDX		FE_REG(0x80C)
#define FE_TDMA_GLO_CFG		FE_REG(0xA04)
#define FE_TDMA_PPE_FC		FE_REG(0x2230)
#define FE_WIFI_PPE_INF		FE_REG(0xFDC)	/* of the FIFO head */
#define FE_WIFI_BUF_ID		FE_REG(0xFE0)
#define FE_WIFI_BUF_CFG		FE_REG(0xFE8)
#define FE_WIFI_CRSN_MSK	FE_REG(0xFEC)

#define TX_CFG_8B_13BIT		0x50000	/* 8-byte descriptors, 13-bit length */
#define BUF_ID_VALID		BIT(31)	/* write it to pop */
#define BUF_ID_BOUND		BIT(30)
#define BUF_ID_ID		GENMASK(15, 0)
#define PPE_INF_FOE		GENMASK(14, 0)
#define PPE_INF_CRSN		GENMASK(20, 16)
#define CRSN_HIT_BIND		0x1F	/* forwarded by a bound entry */
#define TXD_LS			BIT(30)
#define TXD_ID			GENMASK(28, 14)
#define TXD_LEN			GENMASK(12, 0)

#define PPE_RING		1024
#define PPE_SLACK		4	/* the TDMA needs slots beyond its index */
#define PPE_MIN_LEN		60

/* rx task: a slot for one frame, reading the dma index only when out */
static inline bool ppe_room(struct wlan_ppe *p)
{
	u32 dtx;

	if (p->room > PPE_SLACK)
		return true;
	dtx = REG32(FE_TX_DMA_IDX) % PPE_RING;
	p->room = (dtx + PPE_RING - p->idx - 1) % PPE_RING;
	return p->room > PPE_SLACK;
}

/* zero the pad so no stale bytes reach the wire */
static inline void ppe_pad(u8 *frame, u32 len)
{
	while (len < PPE_MIN_LEN)
		frame[len++] = 0;
}

static inline void ppe_submit(struct wlan_ppe *p, u32 id, u32 ofs, u32 len,
			      u32 host_len)
{
	volatile u32 *d = (u32 *)(p->ring + 8 * p->idx);

	if (unlikely(len < PPE_MIN_LEN)) {
		ppe_pad(p->pool + id * LIBRANPU_RX_BUF_SIZE + ofs, len);
		len = PPE_MIN_LEN;
	}
	p->len[id] = host_len;
	d[1] = p->pool_bus + id * LIBRANPU_RX_BUF_SIZE + ofs;
	d[0] = TXD_LS | FIELD_PREP(TXD_ID, id) | FIELD_PREP(TXD_LEN, len);
	p->idx = (p->idx + 1) % PPE_RING;
	p->room--;
	p->pending++;
}

/* once per burst: buffers and descriptors before the index */
static inline void ppe_kick(struct wlan_ppe *p)
{
	if (!p->pending)
		return;
	wmb();
	REG32(FE_TX_CPU_IDX) = p->idx;
	p->pending = 0;
}

int ppe_attach(struct wlan_radio *r);
u32 ppe_take(struct wlan_radio *r, struct id_pool *pool, u32 budget,
	     bool stopping);
u32 ppe_held(struct wlan_radio *r);

#endif

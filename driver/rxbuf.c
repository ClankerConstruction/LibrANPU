// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Rx buffers to the network stack. A frame is built on its NPU buffers
 * and lent to the stack; each pool page keeps a reference bias, and a
 * page whose count falls back to it has no user left but the pool.
 */

#include <linux/dma-mapping.h>
#include <linux/mm.h>
#include <linux/skbuff.h>

#include "libranpu.h"

#define RX_BIAS			USHRT_MAX
/* short frames are cheaper to copy than to lend */
#define RX_COPYBREAK		256
/* lent pages looked at per reclaim call, busy ones go to the back */
#define RX_SCAN			64

static struct page *rxb_page(struct libranpu *npu, u32 p)
{
	return virt_to_page(npu->pool.cpu + p * PAGE_SIZE);
}

static void rxb_ready(struct libranpu_rxb *b, u32 id)
{
	b->ready[(b->ready_head + b->ready_len++) % b->ids] = id;
}

/* the page's lent buffers, if the stack let go of all of them */
static bool rxb_take(struct libranpu *npu, u32 p, u32 *ids, u32 *n)
{
	struct libranpu_rxb *b = &npu->rxb;
	struct libranpu_rx_page *pg = &b->pg[p];
	unsigned long lent = pg->lent;
	u32 slot;

	if (page_ref_count(rxb_page(npu, p)) != 1 + pg->bias)
		return false;
	/* the last user's accesses come before the chip's writes */
	smp_mb();
	for_each_set_bit(slot, &lent, b->per_page) {
		u32 id = p * b->per_page + slot;

		/* no dirty line may land on the chip's next frame */
		dma_sync_single_range_for_device(npu->dev, npu->pool.dma,
						 id * LIBRANPU_RX_BUF_SIZE,
						 b->buf_len, DMA_FROM_DEVICE);
		if (ids)
			ids[(*n)++] = id;
	}
	b->lent -= hweight8(pg->lent);
	b->reclaimed += hweight8(pg->lent);
	pg->lent = 0;
	return true;
}

u32 libranpu_rx_reclaim(struct libranpu *npu, u32 *ids, u32 max)
{
	struct libranpu_rxb *b = &npu->rxb;
	u32 n = 0, scan;

	for (; n < max && b->ready_len; b->ready_len--) {
		ids[n++] = b->ready[b->ready_head];
		b->ready_head = (b->ready_head + 1) % b->ids;
	}

	for (scan = min(b->fifo_len, RX_SCAN);
	     scan && n + b->per_page <= max; scan--) {
		u32 p = b->fifo[b->fifo_head];

		b->fifo_head = (b->fifo_head + 1) % b->pages;
		if (!rxb_take(npu, p, ids, &n))
			b->fifo[(b->fifo_head + b->fifo_len - 1) % b->pages] = p;
		else
			b->fifo_len--;
	}
	return n;
}
EXPORT_SYMBOL_GPL(libranpu_rx_reclaim);

void libranpu_rx_drop(struct libranpu *npu,
		      const struct libranpu_rx_seg *seg, u32 nseg)
{
	u32 i;

	for (i = 0; i < nseg; i++)
		if (seg[i].id < npu->rxb.ids)
			rxb_ready(&npu->rxb, seg[i].id);
}
EXPORT_SYMBOL_GPL(libranpu_rx_drop);

/* the stack takes one of the page's references with the buffer */
static void *rxb_lend(struct libranpu *npu, u32 id)
{
	struct libranpu_rxb *b = &npu->rxb;
	u32 p = id / b->per_page;
	struct libranpu_rx_page *pg = &b->pg[p];

	if (!pg->lent)
		b->fifo[(b->fifo_head + b->fifo_len++) % b->pages] = p;
	pg->lent |= BIT(id % b->per_page);
	if (unlikely(!--pg->bias)) {
		page_ref_add(rxb_page(npu, p), RX_BIAS);
		pg->bias = RX_BIAS;
	}
	b->lent++;
	return npu->pool.cpu + id * LIBRANPU_RX_BUF_SIZE;
}

static struct sk_buff *rxb_build(struct libranpu *npu,
				 const struct libranpu_rx_seg *seg, u32 nseg)
{
	struct sk_buff *skb;
	u32 i;

	skb = napi_build_skb(npu->pool.cpu + seg[0].id * LIBRANPU_RX_BUF_SIZE,
			     LIBRANPU_RX_BUF_SIZE);
	if (!skb)
		return NULL;
	rxb_lend(npu, seg[0].id);
	skb_reserve(skb, seg[0].off);
	__skb_put(skb, seg[0].len);

	for (i = 1; i < nseg; i++) {
		void *va = rxb_lend(npu, seg[i].id) + seg[i].off;

		skb_add_rx_frag(skb, i - 1, virt_to_page(va),
				offset_in_page(va), seg[i].len,
				LIBRANPU_RX_BUF_SIZE);
	}
	return skb;
}

static struct sk_buff *rxb_copy(struct libranpu *npu,
				struct napi_struct *napi,
				const struct libranpu_rx_seg *seg, u32 nseg,
				u32 len)
{
	struct sk_buff *skb = napi_alloc_skb(napi, len);
	u32 i;

	for (i = 0; skb && i < nseg; i++)
		skb_put_data(skb, npu->pool.cpu +
			     seg[i].id * LIBRANPU_RX_BUF_SIZE + seg[i].off,
			     seg[i].len);
	libranpu_rx_drop(npu, seg, nseg);
	return skb;
}

struct sk_buff *libranpu_rx_skb(struct libranpu *npu,
				struct napi_struct *napi,
				const struct libranpu_rx_seg *seg, u32 nseg)
{
	struct libranpu_rxb *b = &npu->rxb;
	struct sk_buff *skb = NULL;
	u32 i, len = 0;

	for (i = 0; i < nseg; i++) {
		if (seg[i].id >= b->ids || !seg[i].len ||
		    seg[i].off + seg[i].len > b->buf_len)
			goto drop;
		dma_sync_single_range_for_cpu(npu->dev, npu->pool.dma,
					      seg[i].id * LIBRANPU_RX_BUF_SIZE +
					      seg[i].off, seg[i].len,
					      DMA_FROM_DEVICE);
		len += seg[i].len;
	}

	if (len > RX_COPYBREAK && b->lent + nseg <= b->lend_max && b->lend) {
		skb = rxb_build(npu, seg, nseg);
		if (skb) {
			b->lent_frames++;
			return skb;
		}
	}
	b->copied_frames++;
	return rxb_copy(npu, napi, seg, nseg, len);

drop:
	libranpu_rx_drop(npu, seg, nseg);
	return NULL;
}
EXPORT_SYMBOL_GPL(libranpu_rx_skb);

/*
 * Before an attach: ids the NPU may take again, and the bitmap of the
 * ones the stack still holds. Returns the bitmap's bus address or 0.
 */
dma_addr_t libranpu_rxb_attach(struct libranpu *npu, u32 rx_ring_ids)
{
	struct libranpu_rxb *b = &npu->rxb;
	u32 i, n = b->fifo_len, held = 0;

	b->ready_len = 0;
	b->lend_max = rx_ring_ids < b->ids ? (b->ids - rx_ring_ids) / 2 : 0;
	if (!b->per_page)
		b->lend_max = 0;

	memset(b->held, 0, BITS_TO_U32(b->ids) * sizeof(u32));
	for (i = 0; i < n; i++) {
		u32 p = b->fifo[b->fifo_head], slot;
		unsigned long lent = b->pg[p].lent;

		b->fifo_head = (b->fifo_head + 1) % b->pages;
		if (rxb_take(npu, p, NULL, NULL)) {
			b->fifo_len--;
			continue;
		}
		b->fifo[(b->fifo_head + b->fifo_len - 1) % b->pages] = p;
		for_each_set_bit(slot, &lent, b->per_page) {
			u32 id = p * b->per_page + slot;

			b->held[id / 32] |= cpu_to_le32(BIT(id % 32));
			held++;
		}
	}
	/* the bitmap is coherent memory: no sync */
	wmb();
	return held ? b->held_dma : 0;
}

/*
 * Lending needs whole 2 KB buffers in each page and the pages' struct
 * page counts; without them every frame is copied.
 */
int libranpu_rxb_init(struct libranpu *npu)
{
	struct libranpu_rxb *b = &npu->rxb;
	phys_addr_t base = virt_to_phys(npu->pool.cpu);
	u32 p;

	b->ids = npu->pool.ids;
	b->lend = true;
	b->buf_len = SKB_WITH_OVERHEAD(LIBRANPU_RX_BUF_SIZE);
	b->ready = kvcalloc(b->ids, sizeof(*b->ready), GFP_KERNEL);
	b->held = dmam_alloc_coherent(npu->dev,
				      BITS_TO_U32(b->ids) * sizeof(u32),
				      &b->held_dma, GFP_KERNEL);
	if (!b->ready || !b->held)
		return -ENOMEM;

	if (PAGE_SIZE / LIBRANPU_RX_BUF_SIZE > BITS_PER_TYPE(u8) ||
	    !PAGE_ALIGNED(base) || b->ids % (PAGE_SIZE / LIBRANPU_RX_BUF_SIZE))
		return 0;

	b->pages = b->ids / (PAGE_SIZE / LIBRANPU_RX_BUF_SIZE);
	b->pg = kvcalloc(b->pages, sizeof(*b->pg), GFP_KERNEL);
	b->fifo = kvcalloc(b->pages, sizeof(*b->fifo), GFP_KERNEL);
	if (!b->pg || !b->fifo)
		return -ENOMEM;

	for (p = 0; p < b->pages; p++) {
		struct page *page = rxb_page(npu, p);
		int ref = page_ref_count(page);

		if (WARN_ON_ONCE(ref < 1))
			return -EINVAL;
		page_ref_add(page, RX_BIAS);
		b->pg[p].bias = RX_BIAS;
		/* still held from an earlier load: lent until let go */
		if (ref > 1) {
			b->pg[p].lent = GENMASK(PAGE_SIZE /
						LIBRANPU_RX_BUF_SIZE - 1, 0);
			b->fifo[b->fifo_len++] = p;
			b->lent += hweight8(b->pg[p].lent);
		}
	}
	b->per_page = PAGE_SIZE / LIBRANPU_RX_BUF_SIZE;
	return 0;
}

/* the stack may still hold pages: give back only the bias */
void libranpu_rxb_deinit(struct libranpu *npu)
{
	struct libranpu_rxb *b = &npu->rxb;
	u32 p;

	for (p = 0; b->pg && p < b->pages; p++)
		page_ref_sub(rxb_page(npu, p), b->pg[p].bias);
	kvfree(b->pg);
	kvfree(b->fifo);
	kvfree(b->ready);
	memset(b, 0, sizeof(*b));
}

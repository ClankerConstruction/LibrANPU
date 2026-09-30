// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * WLAN service: the rx buffer pool, the host adaptor rings and the
 * typed WLAN commands a WiFi driver uses to hand its rings over.
 */

#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>

#include "libranpu.h"

#define HA_STATUS		0x30d030
#define HA_MASK(l)		(0x30d034 + 4 * (l))
#define HA_RX(n)		(0x30d180 + 0x10 * (n))
#define HA_TX(n)		(0x30d080 + 0x10 * (n))
#define HA_BASE			0x0
#define HA_SIZE			0x4
#define HA_PROD			0x8
#define HA_CONS			0xc
#define HA_RINGS		16
#define HA_LINES		3
/* the lines follow the mailbox and the watchdogs when not named */
#define HA_FIRST_IRQ		9

/* LAN to WiFi buffers: only the frame engine and the chip touch them */
static void libranpu_tx_pool_init(struct libranpu *npu)
{
	struct resource res;
	u32 tokens;

	if (of_reserved_mem_region_to_resource_byname(npu->dev->of_node,
						      "tx-pkt", &res))
		return;

	tokens = min_t(u32, resource_size(&res) / LIBRANPU_RX_BUF_SIZE,
		       LIBRANPU_TX_TOKENS);
	if (res.start < NPU_DRAM_WIN_START ||
	    res.start + (u64)tokens * LIBRANPU_RX_BUF_SIZE > NPU_DRAM_WIN_END ||
	    !IS_ALIGNED(res.start, LIBRANPU_RX_BUF_SIZE)) {
		dev_warn(npu->dev, "tx-pkt %pR outside the NPU window\n", &res);
		return;
	}
	npu->tx_pool = res.start;
	npu->tx_tokens = tokens;
}

/*
 * The pool must be ordinary memory the kernel maps: reserved, not
 * no-map, so the CPU reads it cached after an invalidate.
 */
int libranpu_wlan_init(struct libranpu *npu)
{
	struct libranpu_rx_pool *p = &npu->pool;
	struct resource res;
	u32 ids;

	spin_lock_init(&npu->ha_lock);
	mutex_init(&npu->aqm_lock);
	npu->aqm = (struct libranpu_wlan_aqm){
		.on = 1,
		.limit = cpu_to_le32(8192),
		.delay_us = cpu_to_le32(10000),
		.interval_us = cpu_to_le32(100000),
		.min_q = cpu_to_le32(64),
		.small = cpu_to_le32(256),
	};
	libranpu_tx_pool_init(npu);
	npu->ring_tbl = dmam_alloc_coherent(npu->dev, LIBRANPU_WLAN_RINGS *
					    sizeof(*npu->ring_tbl),
					    &npu->ring_tbl_dma, GFP_KERNEL);
	if (!npu->ring_tbl)
		return -ENOMEM;
	if (npu->ring_tbl_dma < NPU_DRAM_WIN_START ||
	    npu->ring_tbl_dma >= NPU_DRAM_WIN_END)
		npu->ring_tbl = NULL;
	if (of_reserved_mem_region_to_resource_byname(npu->dev->of_node,
						      "rx-pkt", &res))
		return 0;

	ids = min_t(u32, resource_size(&res) / LIBRANPU_RX_BUF_SIZE,
		    le32_to_cpu(npu->caps.max_rx_ids) ?: 16384);
	if (!pfn_valid(PHYS_PFN(res.start)) || !ids ||
	    res.start < NPU_DRAM_WIN_START ||
	    res.start + (u64)ids * LIBRANPU_RX_BUF_SIZE > NPU_DRAM_WIN_END) {
		dev_warn(npu->dev, "rx-pkt %pR unusable: no-map or outside the NPU window\n",
			 &res);
		return 0;
	}

	p->cpu = phys_to_virt(res.start);
	p->dma = dma_map_single(npu->dev, p->cpu, ids * LIBRANPU_RX_BUF_SIZE,
				DMA_FROM_DEVICE);
	if (dma_mapping_error(npu->dev, p->dma)) {
		p->cpu = NULL;
		return -ENOMEM;
	}
	p->ids = ids;
	return libranpu_rxb_init(npu);
}

void libranpu_wlan_deinit(struct libranpu *npu)
{
	struct libranpu_rx_pool *p = &npu->pool;

	libranpu_rxb_deinit(npu);
	if (p->cpu)
		dma_unmap_single(npu->dev, p->dma,
				 p->ids * LIBRANPU_RX_BUF_SIZE,
				 DMA_FROM_DEVICE);
	p->cpu = NULL;
}

u32 libranpu_wlan_tx_tokens(struct libranpu *npu)
{
	return npu->tx_tokens;
}
EXPORT_SYMBOL_GPL(libranpu_wlan_tx_tokens);

const struct libranpu_rx_pool *libranpu_rx_pool(struct libranpu *npu)
{
	return npu->pool.cpu ? &npu->pool : NULL;
}
EXPORT_SYMBOL_GPL(libranpu_rx_pool);

static int wlan_ctl(struct libranpu *npu, u16 op, u8 radio, u8 dir,
		    struct libranpu_wlan_audit *audit)
{
	struct libranpu_wlan_ctl c = { .radio = radio, .dir = dir, .on = dir };
	struct libranpu_wlan_audit a = {};
	u16 len = sizeof(a);
	int err;

	err = libranpu_cmd(npu, LIBRANPU_SVC_WLAN, op, &c, sizeof(c), &a,
			   &len);
	if (audit)
		*audit = a;
	return err;
}

int libranpu_wlan_attach(struct libranpu *npu,
			 struct libranpu_wlan_attach *req,
			 struct libranpu_wlan_ring *rings, u32 nrings)
{
	u32 i, ring_ids = 0;
	bool tx = false;
	int err;

	if (!(le32_to_cpu(npu->caps.services) & LIBRANPU_SVC_F_WLAN))
		return -EOPNOTSUPP;
	if (!npu->pool.cpu || !npu->ring_tbl || nrings > LIBRANPU_WLAN_RINGS)
		return -EINVAL;

	for (i = 0; i < nrings; i++) {
		if (rings[i].kind == LIBRANPU_RING_RX_DATA)
			ring_ids += le16_to_cpu(rings[i].entries);
		tx |= rings[i].kind == LIBRANPU_RING_TX_DATA;
	}
	memcpy(npu->ring_tbl, rings, nrings * sizeof(*rings));
	req->nrings = nrings;
	req->ring_table = cpu_to_le32(npu->ring_tbl_dma);
	if (tx && npu->tx_tokens) {
		req->tx_pool_base = cpu_to_le32(npu->tx_pool);
		req->npu_tokens = cpu_to_le16(npu->tx_tokens);
	}
	req->pool_base = cpu_to_le32(npu->pool.dma);
	req->pool_ids = cpu_to_le32(npu->pool.ids);
	/* the stack's skb_shared_info goes after the chip's bytes */
	req->rx_headroom = 0;
	req->rx_buf_len = cpu_to_le16(npu->rxb.buf_len);
	req->rx_held = cpu_to_le32(libranpu_rxb_attach(npu, ring_ids));
	if (READ_ONCE(npu->force_host))
		req->flags |= cpu_to_le32(LIBRANPU_WLAN_F_FORCE_HOST);
	err = libranpu_cmd(npu, LIBRANPU_SVC_WLAN, LIBRANPU_WLAN_ATTACH,
			   req, sizeof(*req), NULL, NULL);
	if (err)
		return err;
	/* the NPU wrote where it placed each chip ring */
	for (i = 0; i < nrings; i++)
		rings[i].base = READ_ONCE(npu->ring_tbl[i].base);

	/* an attach starts from the defaults */
	mutex_lock(&npu->aqm_lock);
	if (libranpu_wlan_aqm_set(npu, &npu->aqm))
		dev_warn(npu->dev, "per-station limit not restored\n");
	mutex_unlock(&npu->aqm_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(libranpu_wlan_attach);

int libranpu_wlan_aqm_set(struct libranpu *npu,
			  const struct libranpu_wlan_aqm *q)
{
	struct libranpu_wlan_aqm req = *q, rsp;
	u16 len = sizeof(rsp);

	req.set = 1;
	return libranpu_cmd(npu, LIBRANPU_SVC_WLAN, LIBRANPU_WLAN_AQM,
			    &req, sizeof(req), &rsp, &len);
}

int libranpu_wlan_start(struct libranpu *npu, u8 radio, u8 dir)
{
	return wlan_ctl(npu, LIBRANPU_WLAN_START, radio, dir, NULL);
}
EXPORT_SYMBOL_GPL(libranpu_wlan_start);

int libranpu_wlan_stop(struct libranpu *npu, u8 radio,
		       struct libranpu_wlan_audit *audit)
{
	return wlan_ctl(npu, LIBRANPU_WLAN_STOP, radio,
			LIBRANPU_WLAN_RX | LIBRANPU_WLAN_TX, audit);
}
EXPORT_SYMBOL_GPL(libranpu_wlan_stop);

int libranpu_wlan_detach(struct libranpu *npu, u8 radio,
			 struct libranpu_wlan_audit *audit)
{
	return wlan_ctl(npu, LIBRANPU_WLAN_DETACH, radio, 0, audit);
}
EXPORT_SYMBOL_GPL(libranpu_wlan_detach);

/* kept for the next attach too, which starts from its own flags */
int libranpu_wlan_force_host(struct libranpu *npu, u8 radio, bool on)
{
	WRITE_ONCE(npu->force_host, on);
	return wlan_ctl(npu, LIBRANPU_WLAN_FORCE_HOST, radio, on, NULL);
}
EXPORT_SYMBOL_GPL(libranpu_wlan_force_host);

int libranpu_wlan_stats(struct libranpu *npu, u8 radio,
			struct libranpu_wlan_stats *stats)
{
	struct libranpu_wlan_ctl c = { .radio = radio };
	u16 len = sizeof(*stats);

	memset(stats, 0, sizeof(*stats));
	return libranpu_cmd(npu, LIBRANPU_SVC_WLAN,
			    LIBRANPU_WLAN_GET_STATS, &c, sizeof(c), stats,
			    &len);
}
EXPORT_SYMBOL_GPL(libranpu_wlan_stats);

int libranpu_wlan_tx_stats(struct libranpu *npu, u8 radio,
			   struct libranpu_wlan_tx_stats *stats)
{
	struct libranpu_wlan_ctl c = { .radio = radio, .page = 1 };
	u16 len = sizeof(*stats);

	memset(stats, 0, sizeof(*stats));
	return libranpu_cmd(npu, LIBRANPU_SVC_WLAN,
			    LIBRANPU_WLAN_GET_STATS, &c, sizeof(c), stats,
			    &len);
}
EXPORT_SYMBOL_GPL(libranpu_wlan_tx_stats);

void libranpu_ha_ring_init(struct libranpu *npu, bool rx, u32 ring,
			   dma_addr_t base, u32 entries)
{
	u32 regs = rx ? HA_RX(ring) : HA_TX(ring);

	if (WARN_ON(ring >= HA_RINGS))
		return;
	npu_wr(npu, regs + HA_BASE, base);
	npu_wr(npu, regs + HA_SIZE, entries);
	npu_wr(npu, regs + HA_PROD, 0);
	npu_wr(npu, regs + HA_CONS, 0);
}
EXPORT_SYMBOL_GPL(libranpu_ha_ring_init);

u32 libranpu_ha_rx_prod(struct libranpu *npu, u32 ring)
{
	return npu_rr(npu, HA_RX(ring) + HA_PROD);
}
EXPORT_SYMBOL_GPL(libranpu_ha_rx_prod);

void libranpu_ha_rx_cons(struct libranpu *npu, u32 ring, u32 idx)
{
	npu_wr(npu, HA_RX(ring) + HA_CONS, idx);
}
EXPORT_SYMBOL_GPL(libranpu_ha_rx_cons);

void libranpu_ha_tx_prod(struct libranpu *npu, u32 ring, u32 idx)
{
	npu_wr(npu, HA_TX(ring) + HA_PROD, idx);
}
EXPORT_SYMBOL_GPL(libranpu_ha_tx_prod);

u32 libranpu_ha_tx_cons(struct libranpu *npu, u32 ring)
{
	return npu_rr(npu, HA_TX(ring) + HA_CONS);
}
EXPORT_SYMBOL_GPL(libranpu_ha_tx_cons);

void __iomem *libranpu_ha_tx_regs(struct libranpu *npu, u32 ring)
{
	return ring < HA_RINGS ? npu->base + HA_TX(ring) : NULL;
}
EXPORT_SYMBOL_GPL(libranpu_ha_tx_regs);

int libranpu_ha_irq(struct libranpu *npu, u32 line)
{
	struct platform_device *pdev = to_platform_device(npu->dev);
	char name[8];
	int irq;

	if (line >= HA_LINES)
		return -EINVAL;
	snprintf(name, sizeof(name), "ha%u", line);
	irq = platform_get_irq_byname_optional(pdev, name);
	if (irq < 0)
		irq = platform_get_irq(pdev, HA_FIRST_IRQ + line);
	return irq;
}
EXPORT_SYMBOL_GPL(libranpu_ha_irq);

void libranpu_ha_irq_enable(struct libranpu *npu, u32 line, u32 rx_ring,
			    bool on)
{
	unsigned long flags;
	u32 mask;

	if (WARN_ON(line >= HA_LINES || rx_ring >= HA_RINGS))
		return;
	spin_lock_irqsave(&npu->ha_lock, flags);
	mask = npu_rr(npu, HA_MASK(line));
	if (on)
		mask |= BIT(16 + rx_ring);
	else
		mask &= ~BIT(16 + rx_ring);
	npu_wr(npu, HA_MASK(line), mask);
	spin_unlock_irqrestore(&npu->ha_lock, flags);
}
EXPORT_SYMBOL_GPL(libranpu_ha_irq_enable);

/* write one to clear: only this ring's bit, never another line's */
void libranpu_ha_irq_ack(struct libranpu *npu, u32 rx_ring)
{
	npu_wr(npu, HA_STATUS, BIT(16 + rx_ring));
}
EXPORT_SYMBOL_GPL(libranpu_ha_irq_ack);

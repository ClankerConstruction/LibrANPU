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
	return 0;
}

void libranpu_wlan_deinit(struct libranpu *npu)
{
	struct libranpu_rx_pool *p = &npu->pool;

	if (p->cpu)
		dma_unmap_single(npu->dev, p->dma,
				 p->ids * LIBRANPU_RX_BUF_SIZE,
				 DMA_FROM_DEVICE);
	p->cpu = NULL;
}

const struct libranpu_rx_pool *libranpu_rx_pool(struct libranpu *npu)
{
	return npu->pool.cpu ? &npu->pool : NULL;
}
EXPORT_SYMBOL_GPL(libranpu_rx_pool);

void libranpu_rx_sync(struct libranpu *npu, u32 id, u32 off, u32 len)
{
	dma_sync_single_range_for_cpu(npu->dev, npu->pool.dma,
				      id * LIBRANPU_RX_BUF_SIZE + off, len,
				      DMA_FROM_DEVICE);
}
EXPORT_SYMBOL_GPL(libranpu_rx_sync);

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
			 const struct libranpu_wlan_attach *req,
			 struct libranpu_wlan_attach_rsp *rsp)
{
	u16 len = sizeof(*rsp);

	if (!(le32_to_cpu(npu->caps.services) & LIBRANPU_SVC_F_WLAN))
		return -EOPNOTSUPP;
	return libranpu_cmd(npu, LIBRANPU_SVC_WLAN, LIBRANPU_WLAN_ATTACH,
			    req, sizeof(*req), rsp, &len);
}
EXPORT_SYMBOL_GPL(libranpu_wlan_attach);

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

int libranpu_wlan_force_host(struct libranpu *npu, u8 radio, bool on)
{
	return wlan_ctl(npu, LIBRANPU_WLAN_FORCE_HOST, radio, on, NULL);
}
EXPORT_SYMBOL_GPL(libranpu_wlan_force_host);

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

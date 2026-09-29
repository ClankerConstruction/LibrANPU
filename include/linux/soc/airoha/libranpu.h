/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * LibrANPU: API for the drivers that use its services.
 */
#ifndef __LIBRANPU_H
#define __LIBRANPU_H

#include <linux/notifier.h>
#include <linux/types.h>
#include <linux/soc/airoha/libranpu_abi.h>

struct libranpu;
struct device;
struct napi_struct;
struct sk_buff;

/* from the "airoha,npu" phandle of @dev */
struct libranpu *libranpu_get(struct device *dev);
void libranpu_put(struct libranpu *npu);

struct device *libranpu_dev(struct libranpu *npu);
const struct libranpu_caps *libranpu_caps(struct libranpu *npu);

/*
 * Run one command and sleep until it completes. @rsp_len is the room
 * in @rsp on entry and the response length on return.
 */
int libranpu_cmd(struct libranpu *npu, u8 service, u16 opcode,
		 const void *req, u16 req_len, void *rsp, u16 *rsp_len);

/* WLAN service (driver/wlan.c) */

/* rx buffers: pool.dma + id * 2048 as the NPU and the chip see them */
struct libranpu_rx_pool {
	void *cpu;
	dma_addr_t dma;
	u32 ids;
};

const struct libranpu_rx_pool *libranpu_rx_pool(struct libranpu *npu);

/* one buffer of a frame, from a host rx ring entry */
struct libranpu_rx_seg {
	u32 id;
	u32 off;
	u32 len;
};

/*
 * The frame as an skb, on its buffers or copied; NULL when bad. Every
 * id comes back through libranpu_rx_reclaim(). One NAPI context only.
 */
struct sk_buff *libranpu_rx_skb(struct libranpu *npu,
				struct napi_struct *napi,
				const struct libranpu_rx_seg *seg, u32 nseg);
void libranpu_rx_drop(struct libranpu *npu,
		      const struct libranpu_rx_seg *seg, u32 nseg);
/* up to max ids the NPU may reuse, for the return ring */
u32 libranpu_rx_reclaim(struct libranpu *npu, u32 *ids, u32 max);

/* fills in the rx pool fields of req */
int libranpu_wlan_attach(struct libranpu *npu,
			 struct libranpu_wlan_attach *req,
			 struct libranpu_wlan_attach_rsp *rsp);
int libranpu_wlan_start(struct libranpu *npu, u8 radio, u8 dir);
int libranpu_wlan_stop(struct libranpu *npu, u8 radio,
		       struct libranpu_wlan_audit *audit);
int libranpu_wlan_detach(struct libranpu *npu, u8 radio,
			 struct libranpu_wlan_audit *audit);
int libranpu_wlan_force_host(struct libranpu *npu, u8 radio, bool on);
int libranpu_wlan_stats(struct libranpu *npu, u8 radio,
			struct libranpu_wlan_stats *stats);

/*
 * Host adaptor rings. rx: NPU to host, tx: host to NPU. Each ring has a
 * producer and a consumer index; a new rx producer index raises the
 * lines whose mask holds the ring.
 */
void libranpu_ha_ring_init(struct libranpu *npu, bool rx, u32 ring,
			   dma_addr_t base, u32 entries);
u32 libranpu_ha_rx_prod(struct libranpu *npu, u32 ring);
void libranpu_ha_rx_cons(struct libranpu *npu, u32 ring, u32 idx);
void libranpu_ha_tx_prod(struct libranpu *npu, u32 ring, u32 idx);
u32 libranpu_ha_tx_cons(struct libranpu *npu, u32 ring);
/* base, size, producer, consumer: the layout of a chip DMA ring's */
void __iomem *libranpu_ha_tx_regs(struct libranpu *npu, u32 ring);
int libranpu_ha_irq(struct libranpu *npu, u32 line);
void libranpu_ha_irq_enable(struct libranpu *npu, u32 line, u32 rx_ring,
			    bool on);
void libranpu_ha_irq_ack(struct libranpu *npu, u32 rx_ring);

enum libranpu_event {
	LIBRANPU_FATAL,			/* data: the hart number */
};

int libranpu_register_notifier(struct libranpu *npu,
			       struct notifier_block *nb);
void libranpu_unregister_notifier(struct libranpu *npu,
				  struct notifier_block *nb);

#endif

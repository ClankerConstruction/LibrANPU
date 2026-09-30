/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef __LIBRANPU_INT_H
#define __LIBRANPU_INT_H

#include <linux/completion.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/soc/airoha/libranpu.h>
#include <linux/wait.h>

/* host view of the NPU window */
#define REG_HART_PC(n)			(0x305000 + (n) * 0x100)
#define REG_BOOT_TRIGGER		0x306000
#define REG_BOOT_CONFIG			0x306004
#define REG_BOOT_BASE(n)		(0x306020 + (n) * 4)
#define REG_MBOX_INT_STS		0x30c000
#define REG_MBQ_CTRL(q, n)		(0x30c030 + (q) * 0x10 + (n) * 4)
#define REG_MIB(n)			(0x30c140 + (n) * 4)
#define MBQ_TO_HOST			8
#define MIB_BOOT_BLOCK			10

/* the window starts at cluster SRAM: NPU address of offset 0 */
#define NPU_WIN_NPU_ADDR		0x3e900000

/* host DRAM the NPU reaches */
#define NPU_DRAM_WIN_START		0x80000000ull
#define NPU_DRAM_WIN_END		0xc0000000ull

/* NPU tokens when the tx-pkt region holds them; the host's come after */
#define LIBRANPU_TX_TOKENS		8192

#define NPU_CMD_TIMEOUT_MS		1000
#define NPU_BOOT_TIMEOUT_MS		2000

struct libranpu_soc {
	u16 id;
	u8 harts;
	u32 cluster_size;
	const char *fw_name;
	const char *name;
};

struct libranpu_slot {
	struct completion done;
	u32 seq;
	s32 status;
	bool busy;
	/* the caller gave up; the answer only frees the slot */
	bool abandoned;
};

/* rx buffers the stack holds, per pool page */
struct libranpu_rx_page {
	u16 bias;			/* page references the driver owns */
	u8 lent;			/* bitmap of its buffers */
};

/* rxbuf.c: one NAPI context uses it, attach runs with that one off */
struct libranpu_rxb {
	struct libranpu_rx_page *pg;
	u16 *fifo;			/* pages with lent buffers */
	u16 *ready;			/* ids to give back to the NPU */
	u32 fifo_head, fifo_len;
	u32 ready_head, ready_len;
	u32 ids, pages, per_page;	/* per_page 0: copy only */
	u32 buf_len;			/* headroom + chip buffer length */
	u32 lent, lend_max;
	bool lend;			/* bench switch, debugfs */
	__le32 *held;			/* attach: ids still lent */
	dma_addr_t held_dma;
	u64 lent_frames, copied_frames, reclaimed;
};

struct libranpu {
	struct device *dev;
	void __iomem *base;
	const struct libranpu_soc *soc;
	struct libranpu_img_hdr hdr;
	struct libranpu_caps caps;

	/* boot block, command ring, event ring: one coherent block */
	void *shm;
	dma_addr_t shm_dma;
	struct libranpu_boot *boot;
	struct libranpu_cmd *cmd;
	struct libranpu_evt *evt;

	/* serialises producers of the command ring */
	struct mutex cmd_lock;
	u32 cmd_prod;
	u32 doorbell;
	struct libranpu_slot slot[LIBRANPU_CMD_ENTRIES];
	/* protects slot[] against the event thread */
	spinlock_t slot_lock;
	wait_queue_head_t slot_wq;
	u32 evt_cons;
	u32 faults_seen;
	bool unhealthy;

	int irq;
	struct libranpu_rx_pool pool;
	struct libranpu_rxb rxb;
	bool force_host;		/* every rx frame to the host */
	/* the per-station limit, sent again after each attach */
	struct mutex aqm_lock;
	struct libranpu_wlan_aqm aqm;
	/* the attach's ring table, coherent: the NPU writes the bases */
	struct libranpu_wlan_ring *ring_tbl;
	dma_addr_t ring_tbl_dma;
	phys_addr_t tx_pool;		/* LAN to WiFi token buffers */
	u32 tx_tokens;
	/* protects the host adaptor line masks */
	spinlock_t ha_lock;
	struct blocking_notifier_head notifier;
	struct devlink *devlink;
	bool dl_params;
	struct dentry *debugfs;
	u16 dbg_wcid;			/* debugfs wlan_sta_q */
	ktime_t boot_time;
};

static inline u32 npu_rr(struct libranpu *npu, u32 reg)
{
	return readl(npu->base + reg);
}

static inline void npu_wr(struct libranpu *npu, u32 reg, u32 val)
{
	writel(val, npu->base + reg);
}

/* cmd.c */
void libranpu_cmd_init(struct libranpu *npu);
irqreturn_t libranpu_mbox_irq(int irq, void *data);
irqreturn_t libranpu_mbox_thread(int irq, void *data);

/* wlan.c */
int libranpu_wlan_init(struct libranpu *npu);
int libranpu_wlan_aqm_set(struct libranpu *npu,
			  const struct libranpu_wlan_aqm *q);
void libranpu_wlan_deinit(struct libranpu *npu);

/* rxbuf.c */
int libranpu_rxb_init(struct libranpu *npu);
void libranpu_rxb_deinit(struct libranpu *npu);
dma_addr_t libranpu_rxb_attach(struct libranpu *npu, u32 rx_ring_ids);

/* devlink.c */
struct libranpu *libranpu_devlink_alloc(struct device *dev);
void libranpu_devlink_free(struct libranpu *npu);
void libranpu_devlink_register(struct libranpu *npu);
void libranpu_devlink_unregister(struct libranpu *npu);

/* debugfs.c */
void libranpu_debugfs_init(struct libranpu *npu);

#endif

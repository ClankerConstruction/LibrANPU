/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * WLAN service: one radio, backend rro31. Tasks own their state; the
 * radio struct holds what control sets up at attach and reads back.
 */
#ifndef __WLAN_WLAN_H
#define __WLAN_WLAN_H

#include "core/pool.h"
#include "core/spsc.h"
#include "core/task.h"
#include "ctl/cmd.h"
#include <linux/soc/airoha/libranpu_abi.h>

#define WLAN_BANDS		2
#define WLAN_MAX_SEGS		7
#define WLAN_RING_MAX		4096
#define WLAN_POOL_MAX		16384
#define WLAN_RX2HOST		1024	/* rx task to host task entries */
#define WLAN_RETQ		512	/* id returns between tasks */

/* rx slot handed to the chip: 1792-byte buffer */
#define WLAN_RX_DESC_CTRL	0x07000100
#define WLAN_RX_DESC_DONE	BIT(31)
/* a completion generation the NPU does not expect first */
#define WLAN_GEN_STALE		0xF0000000

enum wlan_state {
	WLAN_DETACHED,
	WLAN_ATTACHED,
	WLAN_RUNNING,
	WLAN_STOPPING,
	WLAN_STOPPED,
};

/* the tasks that acknowledge a stop */
enum wlan_task {
	WT_RX,
	WT_BUF,
	WT_HOST,
	WT_MAX,
};

/* a chip ring: descriptors in NPU SRAM, registers on the PCIe bus */
struct wlan_ring {
	u32 desc;			/* NPU address, uncached */
	u32 bus;			/* address the chip is given */
	u32 regs;			/* register block; +8 is the cpu index */
	u16 entries;
	u8 band;
	u8 link;
};

/* a host adaptor ring in host memory */
struct wlan_host_ring {
	u32 base;			/* NPU view of the host memory */
	u32 regs;			/* host adaptor ring registers */
	u16 entries;
	u16 entry_size;
};

/* rx and buffer tasks to host task, 8 bytes */
struct wlan_rx_msg {
	u16 id;
	u16 len;			/* 14 bits */
	u32 info;			/* host rx info layout, WRX_LAST */
};

/* bit 31 is SEGS in the host entry: the host task replaces it */
#define WRX_LAST		BIT(31)

struct wlan_ppe {
	u32 ring;			/* NPU SRAM, 8 bytes per slot */
	u32 idx;			/* next slot we fill */
	u32 room;			/* free slots at the last look */
	u32 pending;			/* filled, not yet published */
	volatile u16 *len;		/* per id: the host length */
	u8 *pool;			/* rx buffers, uncached */
};


struct wlan_radio {
	u32 state;			/* control writes */
	u32 epoch;			/* bumped per attach */
	u32 flags;
	u32 pool_base;			/* host physical */
	u32 pool_ids;
	u32 nbands;
	struct wlan_ring rx[WLAN_BANDS];
	struct wlan_ring rxdmad;
	struct wlan_host_ring hrx[WLAN_BANDS];
	struct wlan_host_ring hret;
	u32 mod_frames;
	u32 mod_cycles;

	struct id_pool pool;		/* buffer task, after attach */
	struct spsc *rx2host;		/* struct wlan_rx_msg */
	struct spsc *rx2buf;		/* ids the rx task drops */
	struct spsc *host2buf;		/* ids the host task drops */
	struct spsc *ppe2host;		/* struct wlan_rx_msg, unbound frames */
	struct wlan_ppe ppe;		/* rx task sends, buffer task takes */

	u32 ack[WT_MAX];		/* each task: its view of state */
	struct libranpu_wlan_audit audit;	/* buffer task, when stopped */
	u32 delivered;			/* host task: ids given to the host */
	struct libranpu_wlan_stats stats;
};

extern struct wlan_radio wlan_radio;

int wlan_rx_task(struct task *t, int budget);
int wlan_buf_task(struct task *t, int budget);
int wlan_host_task(struct task *t, int budget);

/* task side: the state, and after it everything control set up for it */
static inline u32 wlan_state(struct wlan_radio *r)
{
	u32 st = READ_ONCE(r->state);

	rmb();
	return st;
}

/* NPU SRAM as the chip sees it */
static inline u32 wlan_bus(u32 npu_addr)
{
	return npu_addr & 0x1FFFFFFF;
}

static inline u32 wlan_pool_bus(const struct wlan_radio *r, u32 id)
{
	return r->pool_base + id * LIBRANPU_RX_BUF_SIZE;
}

extern const struct cmd_service wlan_service;
void wlan_ctl_poll(void);

#endif

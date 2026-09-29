/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Platform layer: everything that differs between the NPU and the
 * QEMU test machine. The rest of the firmware only uses this API.
 */
#ifndef __PLAT_PLAT_H
#define __PLAT_PLAT_H

#include "fw/types.h"
#include "soc.h"
#include "map.h"
#include "plat/regs.h"

/* hart 0, first thing: the host's boot block address, then reset */
u32 plat_boot_block_addr(void);
void plat_reset_bus(void);
/* hart 0 after the reset: interrupt routing, console */
void plat_init(void);

/* NPU view of host DRAM, uncached; NULL outside the NPU window */
void *plat_host_ptr(u64 pa, u32 len);
/* cached and uncached aliases of firmware DRAM */
void *plat_cached(void *p);
void *plat_uncached(void *p);

void plat_dcache_inv(const void *line);
void plat_dcache_wb_inv(const void *line);

u32 plat_cpu_mhz(void);
void plat_putc(char c);
u32 plat_hart_pc(u32 hart);

/* host to NPU doorbell counter, NPU to host event interrupt */
u32 plat_doorbell(void);
void plat_notify_host(void);

/* hart wake source: armed by the hart, raised by another hart */
void plat_wake_arm(u32 hart);
void plat_wake_disarm(u32 hart);
void plat_wake_raise(u32 hart);
u32 plat_wake_ack(u32 hart);

#endif

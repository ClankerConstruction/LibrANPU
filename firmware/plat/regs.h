/* SPDX-License-Identifier: GPL-3.0-only */
/* NPU blocks, offsets from NPU_MMIO_BASE (same on every platform) */
#ifndef __PLAT_REGS_H
#define __PLAT_REGS_H

#define NPU_REG(off)		(NPU_MMIO_BASE + (off))

#define REG_HART_PC(n)		NPU_REG(0x5000 + (n) * 0x100)

#define REG_BOOT_TRIGGER	NPU_REG(0x6000)
#define REG_BOOT_CONFIG		NPU_REG(0x6004)
#define REG_BOOT_BASE(n)	NPU_REG(0x6020 + (n) * 4)

#define REG_MBOX_INT_STS	NPU_REG(0xC000)
#define REG_MBOX_INT_MASK(n)	NPU_REG(0xC004 + (n) * 4)
#define REG_MBQ_CTRL(q, n)	NPU_REG(0xC030 + (q) * 0x10 + (n) * 4)
#define REG_MIB(n)		NPU_REG(0xC140 + (n) * 4)
#define MBQ_TO_HOST		8
#define MIB_BOOT_BLOCK		10

#define REG_RSTCTRL1		NPU_REG(0x11834)
#define RSTCTRL1_NPU_BUS	0x10001788
#define REG_THREAD_ENABLE	NPU_REG(0x0F00)

/* PLIC, per hart banked; sources numbered from 0 here */
#define PLIC_PRIO(src)		(NPU_PLIC_BASE + 4 + (src) * 4)
#define PLIC_ENABLE(w)		(NPU_PLIC_BASE + 0x2000 + (w) * 4)
#define PLIC_MASK(w)		(NPU_PLIC_BASE + 0x3000 + (w) * 4)
#define PLIC_THRESHOLD		(NPU_PLIC_BASE + 0x200000)
#define PLIC_CLAIM		(NPU_PLIC_BASE + 0x200004)
#define PLIC_SRC_MBOX(q)	(8 + (q))

#define SOC_UART_BASE		0x1FBF0000
#define SOC_UART_LSR		(SOC_UART_BASE + 0x14)
#define SOC_UART_THRE		BIT(5)
#define SOC_PLL_CFG		0x1FA201FC

#endif

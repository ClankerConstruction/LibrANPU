/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * QEMU virt: the NPU windows are RAM, at the NPU offsets from
 * 0x8E900000, so the host model sees the layout of the real window.
 */
#ifndef __PLAT_QEMU_MAP_H
#define __PLAT_QEMU_MAP_H

#define NPU_SRAM_BASE		0x8E800000
#define NPU_CLUSTER_BASE	0x8E900000
#define NPU_MMIO_BASE		0x8EC00000
/* the host model plays the frame engine here */
#define NPU_FE_BASE		0x89040000
#define NPU_FE_POP		0	/* RAM: clearing the head pops it */
#define FW_CODE_BASE		0x84000000
#define FW_CODE_SIZE		0x100000

#define QEMU_UART		0x10000000
#define QEMU_CLINT		0x02000000
#define QEMU_RAM_BASE		0x80000000
#define QEMU_RAM_SIZE		0x10000000

#endif

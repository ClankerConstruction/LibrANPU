/* SPDX-License-Identifier: GPL-3.0-only */
/* NPU address map as the harts see it */
#ifndef __PLAT_NPU_MAP_H
#define __PLAT_NPU_MAP_H

#define NPU_SRAM_BASE		0x3E800000
#define NPU_CLUSTER_BASE	0x3E900000
#define NPU_MMIO_BASE		0x1EC00000
#define NPU_PLIC_BASE		0x0C000000
#define NPU_FE_BASE		0x1FB50000
#define NPU_FE_POP		0x80000000	/* written to the FIFO head */
#define FW_CODE_BASE		0x84000000
#define FW_CODE_SIZE		0x100000

#endif

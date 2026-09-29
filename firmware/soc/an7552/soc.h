/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef __SOC_AN7552_H
#define __SOC_AN7552_H

#define SOC_ID			0x7552
#define SOC_HARTS		2
#define SOC_SRAM_SIZE		0x40000
#define SOC_CLUSTER_SIZE	0x4000
#define SOC_DBG_OFFSET		0x3000
#define SOC_PLL_SEL_SHIFT	8
/* PCIe inbound windows onto NPU SRAM: base, then end at +4 */
#define SOC_PCIE_WIN0		0x1FA90038
#define SOC_PCIE_WIN1		0x1FA90030
#define SOC_PLL_FREQS		{ 800, 750, 720, 600 }

#endif

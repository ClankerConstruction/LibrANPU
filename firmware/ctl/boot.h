/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef __CTL_BOOT_H
#define __CTL_BOOT_H

#include "fw/types.h"
#include <linux/soc/airoha/libranpu_abi.h>

/* NULL until the block was found and checked */
extern volatile struct libranpu_boot *boot;

int boot_open(u32 pa);
void boot_ready(int status, u32 harts_up, u32 boot_cycles);
/* any hart, from its trap handler */
void boot_fault(u32 hart, u32 mcause);

#endif

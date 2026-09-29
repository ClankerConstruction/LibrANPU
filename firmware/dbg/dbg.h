/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef __DBG_DBG_H
#define __DBG_DBG_H

#include "fw/types.h"
#include "plat/plat.h"
#include <linux/soc/airoha/libranpu_abi.h>

#define dbgblk ((struct libranpu_dbg_block *) \
		(NPU_CLUSTER_BASE + SOC_DBG_OFFSET))

void dbg_init(void);
/* hart 0 only: the ring has one writer */
void dbg_trace(u16 type, u32 arg0, u32 arg1);

static inline void dbg_hart_state(u32 hart, u32 state)
{
	WRITE_ONCE(dbgblk->hart[hart].state, state);
}

static inline u32 dbg_hart_state_get(u32 hart)
{
	return READ_ONCE(dbgblk->hart[hart].state);
}

#endif

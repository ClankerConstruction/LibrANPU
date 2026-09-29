// SPDX-License-Identifier: GPL-3.0-only
/* Debug block: hart states, task counters, trap records, trace ring */

#include "fw/csr.h"
#include "fw/lib.h"
#include "dbg/dbg.h"

_Static_assert(SOC_DBG_OFFSET + LIBRANPU_DBG_SIZE <= SOC_CLUSTER_SIZE,
	       "debug block outside cluster SRAM");

void dbg_init(void)
{
	struct libranpu_dbg_block *d = dbgblk;

	memset(d, 0, LIBRANPU_DBG_SIZE);
	d->version = LIBRANPU_DBG_VERSION;
	d->size = LIBRANPU_DBG_SIZE;
	d->harts = SOC_HARTS;
	wmb();
	d->magic = LIBRANPU_DBG_MAGIC;
}

void dbg_trace(u16 type, u32 arg0, u32 arg1)
{
	struct libranpu_dbg_block *d = dbgblk;
	u32 i = d->trace_head;
	struct libranpu_dbg_trace *t = &d->trace[i % LIBRANPU_DBG_TRACE];

	t->type = type;
	t->hart = hart_id();
	t->cycles = cycles();
	t->arg0 = arg0;
	t->arg1 = arg1;
	wmb();
	d->trace_head = i + 1;
}

// SPDX-License-Identifier: GPL-3.0-only

#include "fw/csr.h"
#include "core/task.h"
#include "core/trap.h"
#include "ctl/boot.h"
#include "dbg/dbg.h"

struct trap_state {
	u32 catch;
	u32 cause;
};

static struct trap_state trap_st[SOC_HARTS];

void trap_catch(bool on)
{
	struct trap_state *s = &trap_st[hart_id()];

	if (on)
		s->cause = 0;
	barrier();
	s->catch = on;
	barrier();
}

u32 trap_caught(void)
{
	struct trap_state *s = &trap_st[hart_id()];
	u32 c = s->cause;

	s->cause = 0;
	return c;
}

static u32 insn_len(u32 pc)
{
	return (*(volatile u16 *)pc & 3) == 3 ? 4 : 2;
}

/* no printing here: the fault may have hit inside the console */
u32 trap_handler(u32 mcause, u32 mepc, u32 mtval, u32 ra, u32 sp)
{
	u32 hart = hart_id();
	struct libranpu_dbg_hart *rec = &dbgblk->hart[hart];

	if (mcause & MCAUSE_IRQ) {
		/* nothing takes interrupts: mask what fired */
		csr_write(mie, 0);
		return mepc;
	}

	if (trap_st[hart].catch) {
		trap_st[hart].cause = mcause | MCAUSE_IRQ;
		return mepc + insn_len(mepc);
	}

	rec->mcause = mcause;
	rec->mepc = mepc;
	rec->mtval = mtval;
	rec->ra = ra;
	rec->sp = sp;
	rec->task = runner_task(hart);
	wmb();
	rec->state = LIBRANPU_HART_FAULT;
	boot_fault(hart, mcause);

	csr_write(mie, 0);
	for (;;)
		wfi();
}

// SPDX-License-Identifier: GPL-3.0-only
/*
 * Health: turn trap records and stalled heartbeats of the other harts
 * into events. Hart 0 faults reach the host through the boot block.
 */

#include "fw/csr.h"
#include "ctl/ctl.h"
#include "ctl/evt.h"
#include "dbg/dbg.h"
#include "plat/plat.h"

#define HEALTH_PERIOD_US	1000
#define STALL_US		100000

struct hart_health {
	u32 beat;
	u32 since;			/* cycles when beat last moved */
	bool fault_sent;
	bool stall_sent;
};

static struct {
	u32 last;
	struct hart_health h[SOC_HARTS];
} health;

static void health_fault(u32 hart)
{
	const struct libranpu_dbg_hart *r = &dbgblk->hart[hart];
	struct libranpu_evt_fatal f = {
		.hart = hart,
		.task = r->task,
		.mcause = r->mcause,
		.mepc = r->mepc,
		.mtval = r->mtval,
		.ra = r->ra,
		.sp = r->sp,
	};

	dbg_trace(LIBRANPU_TRACE_FAULT, hart, r->mcause);
	evt_post(LIBRANPU_EVT_FATAL, 0, &f, sizeof(f));
	evt_flush(true);
}

static void health_stall(u32 hart, u32 beat)
{
	struct libranpu_evt_stall s = {
		.hart = hart,
		.pc = plat_hart_pc(hart),
		.passes = beat,
	};

	dbg_trace(LIBRANPU_TRACE_STALL, hart, s.pc);
	evt_post(LIBRANPU_EVT_TASK_STALL, 0, &s, sizeof(s));
}

int health_task(struct task *t, int budget)
{
	u32 now = cycles(), mhz = plat_cpu_mhz(), h;
	int n = 0;

	(void)t;
	(void)budget;
	if (now - health.last < HEALTH_PERIOD_US * mhz)
		return 0;
	health.last = now;

	for (h = 1; h < SOC_HARTS; h++) {
		struct hart_health *hh = &health.h[h];
		u32 state = dbg_hart_state_get(h);
		u32 beat = READ_ONCE(dbgblk->hart[h].heartbeat);

		if (state == LIBRANPU_HART_FAULT && !hh->fault_sent) {
			hh->fault_sent = true;
			health_fault(h);
			n++;
		}
		if (state != LIBRANPU_HART_RUN || beat != hh->beat) {
			hh->beat = beat;
			hh->since = now;
			hh->stall_sent = false;
			continue;
		}
		if (!hh->stall_sent && now - hh->since > STALL_US * mhz) {
			hh->stall_sent = true;
			health_stall(h, beat);
			n++;
		}
	}
	return n;
}

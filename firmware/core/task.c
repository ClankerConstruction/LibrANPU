// SPDX-License-Identifier: GPL-3.0-only

#include "fw/csr.h"
#include "fw/errno.h"
#include "core/task.h"
#include "dbg/dbg.h"

struct hart_rt {
	struct task task[TASKS_PER_HART];
	u32 n;
	u32 park;			/* hart 0 writes, the hart reads */
};

static struct hart_rt harts[SOC_HARTS];
static u32 ntasks;

/* one cache line per hart: a store per task call stays local */
struct hart_cur {
	u32 task;
} __aligned(64);

static struct hart_cur cur[SOC_HARTS] __hart_local;

int task_add(u32 hart, u8 id, task_fn run, void *ctx, u16 budget)
{
	struct hart_rt *h = &harts[hart];
	struct task *t;

	if (hart >= SOC_HARTS || h->n == TASKS_PER_HART ||
	    ntasks == LIBRANPU_DBG_TASKS)
		return -ENOSPC;

	t = &h->task[h->n++];
	t->run = run;
	t->ctx = ctx;
	t->budget = budget;
	t->id = id;
	t->hart = hart;
	t->rec = &dbgblk->task[ntasks++];
	t->rec->hart = hart;
	t->rec->id = id;
	dbgblk->ntasks = ntasks;
	dbgblk->hart[hart].ntasks = h->n;
	return 0;
}

u32 task_map(u32 hart)
{
	u32 i, map = 0;

	for (i = 0; i < harts[hart].n; i++)
		map |= BIT(harts[hart].task[i].id);
	return map;
}

u32 runner_task(u32 hart)
{
	return cur[hart].task;
}

u32 task_count(void)
{
	return ntasks;
}

const struct task *task_get(u32 idx)
{
	u32 h;

	for (h = 0; h < SOC_HARTS; h++) {
		if (idx < harts[h].n)
			return &harts[h].task[idx];
		idx -= harts[h].n;
	}
	return NULL;
}

/* a task's counters; the debug record gets stores only */
struct task_acct {
	u32 passes;
	u32 work;
	u64 busy;
	u64 idle;
	u32 errors;
};

static void __noreturn park(u32 hart)
{
	dbg_hart_state(hart, LIBRANPU_HART_PARKED);
	csr_write(mie, 0);
	for (;;)
		wfi();
}

void __noreturn runner(u32 hart)
{
	struct hart_rt *h = &harts[hart];
	struct libranpu_dbg_hart *rec = &dbgblk->hart[hart];
	/* fixed once the harts run: a copy on the cached stack */
	struct task task[TASKS_PER_HART];
	struct task_acct acct[TASKS_PER_HART] = {};
	u32 i, n = h->n, beat = 0;

	for (i = 0; i < n; i++)
		task[i] = h->task[i];
	dbg_hart_state(hart, LIBRANPU_HART_RUN);
	for (;;) {
		if (READ_ONCE(h->park) && hart)
			park(hart);

		for (i = 0; i < n; i++) {
			struct task *t = &task[i];
			struct libranpu_dbg_task *r = t->rec;
			struct task_acct *a = &acct[i];
			u32 t0 = cycles(), dt;
			int w;

			cur[hart].task = t->id;
			w = t->run(t, t->budget);

			dt = cycles() - t0;
			r->passes = ++a->passes;
			if (w > 0) {
				r->work = a->work += w;
				a->busy += dt;
				r->busy_lo = (u32)a->busy;
				r->busy_hi = a->busy >> 32;
			} else {
				a->idle += dt;
				r->idle_lo = (u32)a->idle;
				r->idle_hi = a->idle >> 32;
				if (w < 0)
					r->errors = ++a->errors;
			}
		}
		rec->heartbeat = ++beat;

		/* hart 0 parks last: it answers the command that asked */
		if (!hart && READ_ONCE(h->park))
			park(hart);
	}
}

void runner_park(u32 mask)
{
	u32 h;

	for (h = 0; h < SOC_HARTS; h++)
		if (mask & BIT(h))
			WRITE_ONCE(harts[h].park, 1);
}

u32 runner_parked(void)
{
	u32 h, mask = 0;

	for (h = 0; h < SOC_HARTS; h++)
		if (dbg_hart_state_get(h) == LIBRANPU_HART_PARKED)
			mask |= BIT(h);
	return mask;
}

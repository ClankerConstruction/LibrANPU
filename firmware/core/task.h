/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Task runner: each hart polls its tasks round robin. A task never
 * blocks and returns the work it did, 0 when idle.
 */
#ifndef __CORE_TASK_H
#define __CORE_TASK_H

#include "fw/types.h"
#include "plat/plat.h"
#include <linux/soc/airoha/libranpu_abi.h>

#define TASKS_PER_HART		4

struct task;
typedef int (*task_fn)(struct task *t, int budget);

struct task {
	task_fn run;
	void *ctx;
	struct libranpu_dbg_task *rec;
	u16 budget;
	u8 id;
	u8 hart;
};

/* before the harts start; hart 0 only */
int task_add(u32 hart, u8 id, task_fn run, void *ctx, u16 budget);
u32 task_map(u32 hart);
u32 task_count(void);
const struct task *task_get(u32 idx);

void __noreturn runner(u32 hart);

/* the task a hart runs now, for its trap record */
u32 runner_task(u32 hart);

/* park requests are taken at the top of the runner loop */
void runner_park(u32 mask);
u32 runner_parked(void);

#endif

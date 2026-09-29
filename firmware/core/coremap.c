// SPDX-License-Identifier: GPL-3.0-only
/*
 * Which task runs on which hart. Control and health stay on hart 0;
 * the datapath tasks join here as their services come up.
 */

#include "core/coremap.h"
#include "core/task.h"
#include "ctl/ctl.h"
#include "dbg/probe.h"

int coremap_build(void)
{
	u32 h;
	int err;

	err = task_add(0, LIBRANPU_TASK_CTL, ctl_task, NULL, 8);
	if (!err)
		err = task_add(0, LIBRANPU_TASK_HEALTH, health_task, NULL, 1);
	if (err)
		return err;

#ifdef CONFIG_DBG
	for (h = 0; h < SOC_HARTS && !err; h++)
		err = task_add(h, LIBRANPU_TASK_DBG, probe_task,
			       (void *)(uintptr_t)h, 1);
	if (!err)
		err = probe_init();
#else
	(void)h;
#endif
	return err;
}

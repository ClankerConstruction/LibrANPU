// SPDX-License-Identifier: GPL-3.0-only
/*
 * Which task runs on which hart. Control and health stay on hart 0;
 * the datapath tasks join here as their services come up.
 */

#include "core/coremap.h"
#include "core/task.h"
#include "ctl/ctl.h"
#include "dbg/probe.h"
#include "wlan/wlan.h"

int coremap_build(void)
{
	u32 h;
	int err;

	err = task_add(0, LIBRANPU_TASK_CTL, ctl_task, NULL, 8);
	if (!err)
		err = task_add(0, LIBRANPU_TASK_HEALTH, health_task, NULL, 1);
	if (err)
		return err;

#ifdef CONFIG_WLAN
	/* rro31 two-band: rx, host and buffer each own a hart */
	if (!err)
		err = task_add(1, LIBRANPU_TASK_RX, wlan_rx_task, &wlan_radio, 4);
	if (!err)
		err = task_add(3, LIBRANPU_TASK_HOST, wlan_host_task,
			       &wlan_radio, 4);
	if (!err)
		err = task_add(4, LIBRANPU_TASK_BUF, wlan_buf_task, &wlan_radio,
			       4);
#endif
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

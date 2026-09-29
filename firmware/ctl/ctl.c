// SPDX-License-Identifier: GPL-3.0-only
/* Control service: capabilities, task list, reset; the control task */

#include "fw/csr.h"
#include "fw/errno.h"
#include "fw/lib.h"
#include "ctl/cmd.h"
#include "ctl/ctl.h"
#include "ctl/evt.h"
#include "dbg/dbg.h"
#include "plat/plat.h"
#include "wlan/wlan.h"

/* harts get this long to park before RESET answers anyway */
#define RESET_PARK_US		50000

static struct {
	bool pending;
	struct cmd_ref ref;
	u32 t0;
} reset;

static int ctl_nop(struct cmd_ctx *c)
{
	/* echo: the host checks the payload made the round trip */
	memcpy(c->rsp, c->req, c->len);
	c->rsp_len = c->len;
	return 0;
}

static int ctl_get_caps(struct cmd_ctx *c)
{
	caps_fill((struct libranpu_caps *)c->rsp);
	c->rsp_len = sizeof(struct libranpu_caps);
	return 0;
}

static int ctl_get_tasks(struct cmd_ctx *c)
{
	struct libranpu_task_info *ti = (void *)c->rsp;
	u32 i, n = MIN(task_count(), LIBRANPU_TASK_INFO_MAX);

	for (i = 0; i < n; i++) {
		const struct task *t = task_get(i);

		ti[i].hart = t->hart;
		ti[i].id = t->id;
		ti[i].rsv = 0;
		ti[i].passes = t->rec->passes;
		ti[i].work = t->rec->work;
	}
	c->rsp_len = n * sizeof(*ti);
	return 0;
}

static int ctl_reset(struct cmd_ctx *c)
{
	if (reset.pending)
		return -EBUSY;
	reset.pending = true;
	reset.ref = c->ref;
	reset.t0 = cycles();
	runner_park((BIT(SOC_HARTS) - 1) & ~BIT(0));
	return CMD_ASYNC;
}

/* others parked, or out of time: answer, then hart 0 parks too */
static void ctl_reset_poll(void)
{
	u32 others = (BIT(SOC_HARTS) - 1) & ~BIT(0);
	struct libranpu_reset_rsp rsp;
	u32 parked = runner_parked();

	if ((parked & others) != others &&
	    cycles() - reset.t0 < RESET_PARK_US * plat_cpu_mhz())
		return;

	reset.pending = false;
	rsp.parked = parked | BIT(0);
	dbg_trace(LIBRANPU_TRACE_PARK, rsp.parked, 0);
	cmd_finish(&reset.ref, (parked & others) == others ? 0 : -ETIMEDOUT,
		   &rsp, sizeof(rsp));
	evt_flush(true);
	runner_park(BIT(0));
}

static const struct cmd_handler ctl_handlers[] = {
	{ LIBRANPU_CTL_NOP, 0, ctl_nop },
	{ LIBRANPU_CTL_GET_CAPS, 0, ctl_get_caps },
	{ LIBRANPU_CTL_GET_TASKS, 0, ctl_get_tasks },
	{ LIBRANPU_CTL_RESET, 0, ctl_reset },
};

const struct cmd_service ctl_service = {
	ctl_handlers, ARRAY_SIZE(ctl_handlers)
};

int ctl_task(struct task *t, int budget)
{
	int n;

	(void)t;
	if (reset.pending) {
		ctl_reset_poll();
		return 1;
	}
	n = cmd_poll(budget);
#ifdef CONFIG_WLAN
	wlan_ctl_poll();
#endif
	evt_flush(false);
	return n;
}

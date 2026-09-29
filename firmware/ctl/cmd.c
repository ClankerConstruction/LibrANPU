// SPDX-License-Identifier: GPL-3.0-only

#include "fw/errno.h"
#include "fw/lib.h"
#include "ctl/boot.h"
#include "ctl/cmd.h"
#include "ctl/evt.h"
#include "dbg/dbg.h"
#include "plat/plat.h"
#include "wlan/wlan.h"

/* read cmd_prod at least this often, doorbell or not */
#define CMD_SAFETY_PASSES	1024

struct cmd_ring {
	volatile struct libranpu_cmd *ring;
	u32 mask;
	u32 cons;
	u32 prod;
	u32 doorbell;
	u32 passes;
};

static struct cmd_ring cmds;
static struct cmd_ctx cur;

static const struct cmd_service *const services[] = {
	[LIBRANPU_SVC_CTL] = &ctl_service,
#ifdef CONFIG_WLAN
	[LIBRANPU_SVC_WLAN] = &wlan_service,
#endif
#ifdef CONFIG_DBG
	[LIBRANPU_SVC_DBG] = &dbg_service,
#endif
};

void cmd_init(void *ring, u32 entries)
{
	cmds.ring = ring;
	cmds.mask = entries - 1;
	cmds.cons = cmds.prod = READ_ONCE(boot->cmd_prod);
	cmds.doorbell = plat_doorbell();
}

static const struct cmd_handler *cmd_lookup(u8 service, u16 opcode)
{
	const struct cmd_service *s;
	u32 i;

	if (service >= ARRAY_SIZE(services) || !services[service])
		return NULL;
	s = services[service];
	for (i = 0; i < s->n; i++)
		if (s->h[i].opcode == opcode)
			return &s->h[i];
	return NULL;
}

void cmd_finish(const struct cmd_ref *ref, int status, const void *rsp,
		u16 len)
{
	volatile struct libranpu_cmd *c = &cmds.ring[ref->slot];
	struct libranpu_evt_cmd_done done = {
		.status = status,
		.opcode = ref->opcode,
		.service = ref->service,
	};

	if (status || len > LIBRANPU_CMD_PAYLOAD)
		len = 0;
	if (len)
		copy_words((volatile u32 *)c->payload, rsp, len);
	c->rsp_len = len;
	c->status = status;
	wmb();

	dbg_trace(LIBRANPU_TRACE_CMD, ref->service << 16 | ref->opcode,
		  status);
	if (!(ref->flags & LIBRANPU_CMD_F_NO_EVENT))
		evt_post(LIBRANPU_EVT_CMD_DONE, ref->seq, &done,
			 sizeof(done));
}

static void cmd_run(u32 slot)
{
	volatile struct libranpu_cmd *c = &cmds.ring[slot];
	const struct cmd_handler *h;
	int ret;

	cur.ref.slot = slot;
	cur.ref.seq = c->seq;
	cur.ref.opcode = c->opcode;
	cur.ref.service = c->service;
	cur.ref.flags = c->flags;
	cur.len = c->len;
	cur.rsp_len = 0;

	h = cmd_lookup(cur.ref.service, cur.ref.opcode);
	if (!h) {
		ret = -EOPNOTSUPP;
	} else if (cur.len > LIBRANPU_CMD_PAYLOAD || cur.len < h->min_len) {
		ret = -EINVAL;
	} else {
		memset(cur.req, 0, sizeof(cur.req));
		copy_words(cur.req, (volatile u32 *)c->payload, cur.len);
		ret = h->fn(&cur);
		if (ret == CMD_ASYNC)
			return;
	}
	cmd_finish(&cur.ref, ret, cur.rsp, cur.rsp_len);
}

int cmd_poll(int budget)
{
	u32 db = plat_doorbell();
	int n = 0;

	if (!cmds.ring)
		return 0;

	if (db != cmds.doorbell || ++cmds.passes >= CMD_SAFETY_PASSES ||
	    cmds.cons != cmds.prod) {
		cmds.doorbell = db;
		cmds.passes = 0;
		cmds.prod = READ_ONCE(boot->cmd_prod);
		rmb();
	}

	/* never more than the ring holds: the host index is not trusted */
	if (cmds.prod - cmds.cons > cmds.mask + 1)
		cmds.prod = cmds.cons + cmds.mask + 1;

	while (cmds.cons != cmds.prod && n < budget) {
		cmd_run(cmds.cons & cmds.mask);
		cmds.cons++;
		n++;
	}
	if (n)
		WRITE_ONCE(boot->cmd_cons, cmds.cons);
	return n;
}

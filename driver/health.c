// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * devlink health reporter "fw": traps, stalled harts and command
 * timeouts, with the debug block in the dump.
 */

#include <linux/slab.h>
#include <net/devlink.h>

#include "libranpu.h"

static const char * const fault_names[] = {
	[LIBRANPU_FAULT_TRAP] = "trap",
	[LIBRANPU_FAULT_STALL] = "stall",
	[LIBRANPU_FAULT_CMD_TIMEOUT] = "command timeout",
};

static const char * const hart_states[] = {
	[LIBRANPU_HART_OFF] = "off",
	[LIBRANPU_HART_INIT] = "init",
	[LIBRANPU_HART_RUN] = "run",
	[LIBRANPU_HART_PARKED] = "parked",
	[LIBRANPU_HART_FAULT] = "fault",
};

static const char *hart_state(u32 s)
{
	return s < ARRAY_SIZE(hart_states) ? hart_states[s] : "?";
}

static void fault_put(struct devlink_fmsg *fmsg, const struct libranpu_fault *f)
{
	devlink_fmsg_pair_nest_start(fmsg, "fault");
	devlink_fmsg_obj_nest_start(fmsg);
	devlink_fmsg_string_pair_put(fmsg, "kind", fault_names[f->kind]);
	devlink_fmsg_u32_pair_put(fmsg, "hart", f->hart);
	devlink_fmsg_u32_pair_put(fmsg, "task", f->task);
	devlink_fmsg_u32_pair_put(fmsg, "cause", f->cause);
	devlink_fmsg_u32_pair_put(fmsg, "pc", f->pc);
	devlink_fmsg_obj_nest_end(fmsg);
	devlink_fmsg_pair_nest_end(fmsg);
}

static void harts_put(struct libranpu *npu, struct devlink_fmsg *fmsg,
		      const struct libranpu_dbg_block *d)
{
	u32 h;

	devlink_fmsg_arr_pair_nest_start(fmsg, "harts");
	for (h = 0; h < npu->soc->harts; h++) {
		const struct libranpu_dbg_hart *r = &d->hart[h];

		devlink_fmsg_obj_nest_start(fmsg);
		devlink_fmsg_u32_pair_put(fmsg, "hart", h);
		devlink_fmsg_string_pair_put(fmsg, "state",
					     hart_state(le32_to_cpu(r->state)));
		devlink_fmsg_u32_pair_put(fmsg, "pc",
					  npu_rr(npu, REG_HART_PC(h)));
		devlink_fmsg_u32_pair_put(fmsg, "heartbeat",
					  le32_to_cpu(r->heartbeat));
		devlink_fmsg_u32_pair_put(fmsg, "boot_fault",
					  le32_to_cpu(READ_ONCE(npu->boot->fault[h])));
		devlink_fmsg_u32_pair_put(fmsg, "mcause", le32_to_cpu(r->mcause));
		devlink_fmsg_u32_pair_put(fmsg, "mepc", le32_to_cpu(r->mepc));
		devlink_fmsg_u32_pair_put(fmsg, "mtval", le32_to_cpu(r->mtval));
		devlink_fmsg_u32_pair_put(fmsg, "ra", le32_to_cpu(r->ra));
		devlink_fmsg_u32_pair_put(fmsg, "sp", le32_to_cpu(r->sp));
		devlink_fmsg_u32_pair_put(fmsg, "task", le32_to_cpu(r->task));
		devlink_fmsg_obj_nest_end(fmsg);
	}
	devlink_fmsg_arr_pair_nest_end(fmsg);
}

static void tasks_put(struct devlink_fmsg *fmsg,
		      const struct libranpu_dbg_block *d)
{
	u32 i, n = min_t(u32, le32_to_cpu(d->ntasks), LIBRANPU_DBG_TASKS);

	devlink_fmsg_arr_pair_nest_start(fmsg, "tasks");
	for (i = 0; i < n; i++) {
		const struct libranpu_dbg_task *t = &d->task[i];

		devlink_fmsg_obj_nest_start(fmsg);
		devlink_fmsg_u32_pair_put(fmsg, "hart", t->hart);
		devlink_fmsg_u32_pair_put(fmsg, "id", t->id);
		devlink_fmsg_u32_pair_put(fmsg, "passes", le32_to_cpu(t->passes));
		devlink_fmsg_u32_pair_put(fmsg, "work", le32_to_cpu(t->work));
		devlink_fmsg_u32_pair_put(fmsg, "errors", le32_to_cpu(t->errors));
		devlink_fmsg_obj_nest_end(fmsg);
	}
	devlink_fmsg_arr_pair_nest_end(fmsg);
}

/* the trace ring, oldest entry first */
static void trace_put(struct devlink_fmsg *fmsg,
		      const struct libranpu_dbg_block *d)
{
	u32 head = le32_to_cpu(d->trace_head), i;
	u32 n = min_t(u32, head, LIBRANPU_DBG_TRACE);

	devlink_fmsg_arr_pair_nest_start(fmsg, "trace");
	for (i = head - n; i != head; i++) {
		const struct libranpu_dbg_trace *t =
			&d->trace[i % LIBRANPU_DBG_TRACE];

		devlink_fmsg_obj_nest_start(fmsg);
		devlink_fmsg_u32_pair_put(fmsg, "type", le16_to_cpu(t->type));
		devlink_fmsg_u32_pair_put(fmsg, "hart", t->hart);
		devlink_fmsg_u32_pair_put(fmsg, "cycles", le32_to_cpu(t->cycles));
		devlink_fmsg_u32_pair_put(fmsg, "arg0", le32_to_cpu(t->arg0));
		devlink_fmsg_u32_pair_put(fmsg, "arg1", le32_to_cpu(t->arg1));
		devlink_fmsg_obj_nest_end(fmsg);
	}
	devlink_fmsg_arr_pair_nest_end(fmsg);
}

static int libranpu_fw_dump(struct devlink_health_reporter *reporter,
			    struct devlink_fmsg *fmsg, void *priv_ctx,
			    struct netlink_ext_ack *extack)
{
	struct libranpu *npu = devlink_health_reporter_priv(reporter);
	struct libranpu_dbg_block *d;

	d = kmalloc(sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	memcpy_fromio(d, npu->base + le32_to_cpu(npu->hdr.dbg_offset),
		      sizeof(*d));

	if (priv_ctx)
		fault_put(fmsg, priv_ctx);
	harts_put(npu, fmsg, d);
	tasks_put(fmsg, d);
	trace_put(fmsg, d);
	kfree(d);
	return 0;
}

static int libranpu_fw_diagnose(struct devlink_health_reporter *reporter,
				struct devlink_fmsg *fmsg,
				struct netlink_ext_ack *extack)
{
	struct libranpu *npu = devlink_health_reporter_priv(reporter);

	devlink_fmsg_bool_pair_put(fmsg, "healthy", !READ_ONCE(npu->unhealthy));
	devlink_fmsg_u32_pair_put(fmsg, "faulted_harts", npu->faults_seen);
	devlink_fmsg_u32_pair_put(fmsg, "fw_version",
				  le32_to_cpu(npu->caps.fw_version));
	devlink_fmsg_u64_pair_put(fmsg, "up_ms",
				  ktime_ms_delta(ktime_get(), npu->boot_time));
	return 0;
}

/* debug images: a trap on the last hart, as a real one would come */
static int libranpu_fw_test(struct devlink_health_reporter *reporter,
			    struct netlink_ext_ack *extack)
{
	struct libranpu *npu = devlink_health_reporter_priv(reporter);
	struct libranpu_dbg_probe p = {
		.hart = npu->soc->harts - 1,
		.probe = LIBRANPU_PROBE_TRAP,
	};
	struct libranpu_dbg_probe_rsp rsp;
	u16 len = sizeof(rsp);
	int err;

	if (!(le32_to_cpu(npu->caps.services) & LIBRANPU_SVC_F_DBG)) {
		NL_SET_ERR_MSG_MOD(extack, "firmware without the debug service");
		return -EOPNOTSUPP;
	}
	err = libranpu_cmd(npu, LIBRANPU_SVC_DBG, LIBRANPU_DBG_PROBE, &p,
			   sizeof(p), &rsp, &len);
	/* the trapped hart never answers: the probe times out */
	return err == -ETIMEDOUT ? 0 : err;
}

static const struct devlink_health_reporter_ops libranpu_fw_ops = {
	.name = "fw",
	.dump = libranpu_fw_dump,
	.diagnose = libranpu_fw_diagnose,
	.test = libranpu_fw_test,
};

static void libranpu_health_work(struct work_struct *work)
{
	struct libranpu *npu = container_of(work, struct libranpu, health_work);
	struct devlink_health_reporter *r;
	struct libranpu_fault f;
	char msg[64];

	/* fini waits for us once it has cleared the reporter */
	spin_lock_bh(&npu->health_lock);
	r = npu->fw_reporter;
	f = npu->fault;
	spin_unlock_bh(&npu->health_lock);
	if (!r)
		return;

	snprintf(msg, sizeof(msg), "hart %u %s", f.hart, fault_names[f.kind]);
	devlink_health_report(r, msg, &f);
}

/* any context: devlink reports from a work item */
void libranpu_health_fault(struct libranpu *npu, enum libranpu_fault_kind kind,
			   u8 hart, u8 task, u32 cause, u32 pc)
{
	spin_lock_bh(&npu->health_lock);
	if (npu->fw_reporter) {
		npu->fault = (struct libranpu_fault) {
			.kind = kind, .hart = hart, .task = task,
			.cause = cause, .pc = pc,
		};
		queue_work(system_unbound_wq, &npu->health_work);
	}
	spin_unlock_bh(&npu->health_lock);
}

void libranpu_health_init(struct libranpu *npu)
{
	struct devlink_health_reporter *r;

	spin_lock_init(&npu->health_lock);
	INIT_WORK(&npu->health_work, libranpu_health_work);
	r = devlink_health_reporter_create(npu->devlink, &libranpu_fw_ops, npu);
	if (IS_ERR(r)) {
		dev_warn(npu->dev, "no health reporter: %ld\n", PTR_ERR(r));
		return;
	}
	spin_lock_bh(&npu->health_lock);
	npu->fw_reporter = r;
	spin_unlock_bh(&npu->health_lock);
}

void libranpu_health_fini(struct libranpu *npu)
{
	struct devlink_health_reporter *r;

	spin_lock_bh(&npu->health_lock);
	r = npu->fw_reporter;
	npu->fw_reporter = NULL;
	spin_unlock_bh(&npu->health_lock);
	if (!r)
		return;
	cancel_work_sync(&npu->health_work);
	devlink_health_reporter_destroy(r);
}

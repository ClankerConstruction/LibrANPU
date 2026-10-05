// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Command and event rings. A command's sequence number is its ring
 * index, so the slot of a completion is known without a search.
 */

#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/slab.h>

#include "libranpu.h"

#define CMD_MASK	(LIBRANPU_CMD_ENTRIES - 1)
#define EVT_MASK	(LIBRANPU_EVT_ENTRIES - 1)

void libranpu_cmd_init(struct libranpu *npu)
{
	int i;

	mutex_init(&npu->cmd_lock);
	spin_lock_init(&npu->slot_lock);
	init_waitqueue_head(&npu->slot_wq);
	BLOCKING_INIT_NOTIFIER_HEAD(&npu->notifier);
	for (i = 0; i < LIBRANPU_CMD_ENTRIES; i++)
		init_completion(&npu->slot[i].done);
}

static bool slot_free(struct libranpu *npu, u32 idx)
{
	bool busy;

	spin_lock_bh(&npu->slot_lock);
	busy = npu->slot[idx].busy;
	spin_unlock_bh(&npu->slot_lock);
	return !busy;
}

/* Returns the slot, holding cmd_lock, once the next ring slot is free */
static int cmd_claim(struct libranpu *npu, u32 *idx)
{
	long ret;

	for (;;) {
		mutex_lock(&npu->cmd_lock);
		if (npu->unhealthy) {
			mutex_unlock(&npu->cmd_lock);
			return -EIO;
		}
		*idx = npu->cmd_prod & CMD_MASK;
		if (slot_free(npu, *idx))
			return 0;
		mutex_unlock(&npu->cmd_lock);

		ret = wait_event_interruptible_timeout(npu->slot_wq,
						       slot_free(npu, *idx) ||
						       READ_ONCE(npu->unhealthy),
						       msecs_to_jiffies(NPU_CMD_TIMEOUT_MS));
		if (ret < 0)
			return ret;
		if (!ret)
			return -EBUSY;
	}
}

static void cmd_release(struct libranpu *npu, u32 idx)
{
	spin_lock_bh(&npu->slot_lock);
	npu->slot[idx].busy = false;
	npu->slot[idx].abandoned = false;
	spin_unlock_bh(&npu->slot_lock);
	wake_up(&npu->slot_wq);
}

int libranpu_cmd(struct libranpu *npu, u8 service, u16 opcode,
		 const void *req, u16 req_len, void *rsp, u16 *rsp_len)
{
	struct libranpu_slot *s;
	struct libranpu_cmd *c;
	u16 room = rsp_len ? *rsp_len : 0;
	u32 idx, seq, len;
	int err;

	if (req_len > LIBRANPU_CMD_PAYLOAD || (room && !rsp))
		return -EINVAL;

	err = cmd_claim(npu, &idx);
	if (err)
		return err;

	seq = npu->cmd_prod;
	s = &npu->slot[idx];
	c = &npu->cmd[idx];

	memset(c, 0, sizeof(*c));
	c->opcode = cpu_to_le16(opcode);
	c->service = service;
	c->seq = cpu_to_le32(seq);
	c->len = cpu_to_le16(req_len);
	if (req_len)
		memcpy(c->payload, req, req_len);

	reinit_completion(&s->done);
	spin_lock_bh(&npu->slot_lock);
	s->seq = seq;
	s->busy = true;
	spin_unlock_bh(&npu->slot_lock);

	/* the slot before the index, the index before the doorbell */
	dma_wmb();
	WRITE_ONCE(npu->boot->cmd_prod, cpu_to_le32(++npu->cmd_prod));
	npu_wr(npu, REG_MBQ_CTRL(0, 2), ++npu->doorbell);
	mutex_unlock(&npu->cmd_lock);

	if (!wait_for_completion_timeout(&s->done,
					 msecs_to_jiffies(NPU_CMD_TIMEOUT_MS))) {
		bool late;

		/* the NPU may still write the slot: keep it until it answers */
		spin_lock_bh(&npu->slot_lock);
		late = try_wait_for_completion(&s->done);
		s->abandoned = !late;
		spin_unlock_bh(&npu->slot_lock);
		if (!late) {
			u32 pc = npu_rr(npu, REG_HART_PC(0));

			dev_err(npu->dev, "command %u/%u timed out, hart0 pc %08x\n",
				service, opcode, pc);
			WRITE_ONCE(npu->unhealthy, true);
			wake_up(&npu->slot_wq);
			libranpu_health_fault(npu, LIBRANPU_FAULT_CMD_TIMEOUT, 0, 0,
					      service << 16 | opcode, pc);
			return -ETIMEDOUT;
		}
	}

	dma_rmb();
	err = s->status;
	len = min_t(u32, le16_to_cpu(c->rsp_len), LIBRANPU_CMD_PAYLOAD);
	if (!err && rsp_len) {
		if (len > room)
			err = -EOVERFLOW;
		else
			memcpy(rsp, c->payload, len);
		*rsp_len = len;
	}
	cmd_release(npu, idx);
	return err;
}
EXPORT_SYMBOL_GPL(libranpu_cmd);

static void evt_cmd_done(struct libranpu *npu,
			 const struct libranpu_evt *e)
{
	const struct libranpu_evt_cmd_done *d = (const void *)e->payload;
	u32 seq = le32_to_cpu(e->seq), idx = seq & CMD_MASK;
	struct libranpu_slot *s = &npu->slot[idx];
	bool abandoned = false, ok = false;
	s32 status;

	if (le16_to_cpu(e->len) < sizeof(*d)) {
		dev_warn_ratelimited(npu->dev, "short CMD_DONE\n");
		return;
	}
	status = le32_to_cpu(d->status);
	/* only errnos pass: a stray value must not look like success */
	if (status > 0 || status < -MAX_ERRNO)
		status = -EIO;

	spin_lock_bh(&npu->slot_lock);
	if (s->busy && s->seq == seq) {
		ok = true;
		s->status = status;
		abandoned = s->abandoned;
		if (!abandoned)
			complete(&s->done);
	}
	spin_unlock_bh(&npu->slot_lock);

	if (!ok)
		dev_warn_ratelimited(npu->dev, "CMD_DONE for idle seq %u\n", seq);
	else if (abandoned)
		cmd_release(npu, idx);
}

/*
 * A trap reaches us twice, as a FATAL event and as the boot block fault
 * word (the only way for hart 0): act on whichever comes first.
 */
static void hart_fault(struct libranpu *npu, u32 hart)
{
	struct libranpu_dbg_hart r;

	if (hart >= npu->soc->harts || npu->faults_seen & BIT(hart))
		return;
	npu->faults_seen |= BIT(hart);

	memcpy_fromio(&r, npu->base + le32_to_cpu(npu->hdr.dbg_offset) +
		      offsetof(struct libranpu_dbg_block, hart[hart]), sizeof(r));
	dev_err(npu->dev,
		"hart %u task %u fault: mcause %x mepc %08x mtval %08x ra %08x sp %08x\n",
		hart, le32_to_cpu(r.task), le32_to_cpu(r.mcause),
		le32_to_cpu(r.mepc), le32_to_cpu(r.mtval), le32_to_cpu(r.ra),
		le32_to_cpu(r.sp));
	WRITE_ONCE(npu->unhealthy, true);
	wake_up(&npu->slot_wq);
	libranpu_health_fault(npu, LIBRANPU_FAULT_TRAP, hart,
			      le32_to_cpu(r.task), le32_to_cpu(r.mcause),
			      le32_to_cpu(r.mepc));
	blocking_notifier_call_chain(&npu->notifier, LIBRANPU_FATAL,
				     (void *)(uintptr_t)hart);
}

static void evt_fatal(struct libranpu *npu, const struct libranpu_evt *e)
{
	const struct libranpu_evt_fatal *f = (const void *)e->payload;

	if (le16_to_cpu(e->len) >= sizeof(*f))
		hart_fault(npu, f->hart);
}

static void evt_stall(struct libranpu *npu, const struct libranpu_evt *e)
{
	const struct libranpu_evt_stall *st = (const void *)e->payload;

	if (le16_to_cpu(e->len) < sizeof(*st))
		return;
	dev_warn(npu->dev, "hart %u stalled at pc %08x after %u passes\n",
		 st->hart, le32_to_cpu(st->pc), le32_to_cpu(st->passes));
	libranpu_health_fault(npu, LIBRANPU_FAULT_STALL, st->hart, 0, 0,
			      le32_to_cpu(st->pc));
}

static void evt_one(struct libranpu *npu, const struct libranpu_evt *e)
{
	u16 type = le16_to_cpu(e->type);

	if (le16_to_cpu(e->len) > LIBRANPU_EVT_PAYLOAD) {
		dev_warn_ratelimited(npu->dev, "event %u: bad length\n", type);
		return;
	}

	switch (type) {
	case LIBRANPU_EVT_CMD_DONE:
		evt_cmd_done(npu, e);
		break;
	case LIBRANPU_EVT_FATAL:
		evt_fatal(npu, e);
		break;
	case LIBRANPU_EVT_TASK_STALL:
		evt_stall(npu, e);
		break;
	default:
		dev_dbg(npu->dev, "event %u ignored\n", type);
		break;
	}
}

static void check_faults(struct libranpu *npu)
{
	u32 h;

	for (h = 0; h < npu->soc->harts; h++)
		if (le32_to_cpu(READ_ONCE(npu->boot->fault[h])) &
		    LIBRANPU_FAULT_VALID)
			hart_fault(npu, h);
}

irqreturn_t libranpu_mbox_irq(int irq, void *data)
{
	struct libranpu *npu = data;

	npu_wr(npu, REG_MBOX_INT_STS, BIT(MBQ_TO_HOST));
	return IRQ_WAKE_THREAD;
}

/*
 * Drain to the NPU's index, publish ours, then look again: an event
 * posted meanwhile raised no interrupt, since we had not caught up.
 */
irqreturn_t libranpu_mbox_thread(int irq, void *data)
{
	struct libranpu *npu = data;
	u32 prod;

	check_faults(npu);
	for (;;) {
		prod = le32_to_cpu(READ_ONCE(npu->boot->evt_prod));
		dma_rmb();
		if (prod - npu->evt_cons > LIBRANPU_EVT_ENTRIES) {
			dev_err_ratelimited(npu->dev, "event index %u out of range\n",
					    prod);
			return IRQ_HANDLED;
		}
		while (npu->evt_cons != prod) {
			struct libranpu_evt e = npu->evt[npu->evt_cons & EVT_MASK];

			evt_one(npu, &e);
			npu->evt_cons++;
		}
		WRITE_ONCE(npu->boot->evt_cons, cpu_to_le32(npu->evt_cons));
		/* our index out before we look at theirs */
		mb();
		if (le32_to_cpu(READ_ONCE(npu->boot->evt_prod)) == prod)
			return IRQ_HANDLED;
	}
}

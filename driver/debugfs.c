// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * debugfs, bench use only: raw debug block, hart and task state, and a
 * hook to run the firmware's hardware probes.
 */

#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "libranpu.h"

static const char *const hart_states[] = {
	"off", "init", "run", "parked", "fault",
};

static const char *const task_names[] = {
	"ctl", "health", "dbg", "rx", "tx", "buf", "host", "svc", "dba",
};

#define DBG_FIELD(npu, f) \
	readl((npu)->base + le32_to_cpu((npu)->hdr.dbg_offset) + \
	      offsetof(struct libranpu_dbg_block, f))

static int status_show(struct seq_file *s, void *data)
{
	struct libranpu *npu = s->private;
	u32 h, t, n;

	seq_printf(s, "ready in %lld us, harts up %x, %u MHz, events %u\n",
		   ktime_to_us(npu->boot_time),
		   le32_to_cpu(npu->boot->harts_up),
		   le16_to_cpu(npu->caps.cpu_mhz), npu->evt_cons);
	seq_printf(s, "boot cycles %u, unhealthy %d\n",
		   le32_to_cpu(npu->boot->boot_cycles), npu->unhealthy);

	for (h = 0; h < npu->hdr.harts; h++) {
		u32 st = DBG_FIELD(npu, hart[h].state);

		seq_printf(s, "hart%u: %-6s beat %10u pc %08x",
			   h, st < ARRAY_SIZE(hart_states) ? hart_states[st] : "?",
			   DBG_FIELD(npu, hart[h].heartbeat),
			   npu_rr(npu, REG_HART_PC(h)));
		if (st == LIBRANPU_HART_FAULT)
			seq_printf(s, " mcause %x mepc %08x mtval %08x",
				   DBG_FIELD(npu, hart[h].mcause),
				   DBG_FIELD(npu, hart[h].mepc),
				   DBG_FIELD(npu, hart[h].mtval));
		seq_putc(s, '\n');
	}

	n = min_t(u32, DBG_FIELD(npu, ntasks), LIBRANPU_DBG_TASKS);
	for (t = 0; t < n; t++) {
		u32 w = readl(npu->base + le32_to_cpu(npu->hdr.dbg_offset) +
			      offsetof(struct libranpu_dbg_block, task[t]));
		u32 id = (w >> 8) & 0xff;

		seq_printf(s, "task%-2u hart%u %-6s passes %10u work %10u busy %u:%08x idle %u:%08x\n",
			   t, w & 0xff,
			   id < ARRAY_SIZE(task_names) ? task_names[id] : "?",
			   DBG_FIELD(npu, task[t].passes),
			   DBG_FIELD(npu, task[t].work),
			   DBG_FIELD(npu, task[t].busy_hi),
			   DBG_FIELD(npu, task[t].busy_lo),
			   DBG_FIELD(npu, task[t].idle_hi),
			   DBG_FIELD(npu, task[t].idle_lo));
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(status);

static ssize_t dbg_block_read(struct file *file, char __user *buf, size_t len,
			      loff_t *ppos)
{
	struct libranpu *npu = file->private_data;
	ssize_t ret;
	void *blk;

	blk = kmalloc(LIBRANPU_DBG_SIZE, GFP_KERNEL);
	if (!blk)
		return -ENOMEM;
	memcpy_fromio(blk, npu->base + le32_to_cpu(npu->hdr.dbg_offset),
		      LIBRANPU_DBG_SIZE);
	ret = simple_read_from_buffer(buf, len, ppos, blk, LIBRANPU_DBG_SIZE);
	kfree(blk);
	return ret;
}

static const struct file_operations dbg_block_fops = {
	.open = simple_open,
	.read = dbg_block_read,
	.llseek = default_llseek,
};

/* "HART PROBE [ARG0 [ARG1 [ARG2]]]" runs one probe; read shows it */
struct probe_state {
	/* one probe at a time, and its result */
	struct mutex lock;
	char result[160];
};

static struct probe_state probe_st = {
	.lock = __MUTEX_INITIALIZER(probe_st.lock),
};

static ssize_t probe_write(struct file *file, const char __user *ubuf,
			   size_t len, loff_t *ppos)
{
	struct libranpu *npu = file->private_data;
	struct libranpu_dbg_probe p = {};
	struct libranpu_dbg_probe_rsp r = {};
	u32 hart, id, a[3] = {};
	u16 rlen = sizeof(r);
	char buf[80];
	int n, err;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = 0;

	n = sscanf(buf, "%u %u %i %i %i", &hart, &id, &a[0], &a[1], &a[2]);
	if (n < 2 || hart > 255 || id > 255)
		return -EINVAL;
	p.hart = hart;
	p.probe = id;
	for (n = 0; n < 3; n++)
		p.arg[n] = cpu_to_le32(a[n]);

	err = libranpu_cmd(npu, LIBRANPU_SVC_DBG, LIBRANPU_DBG_PROBE,
			   &p, sizeof(p), &r, &rlen);

	mutex_lock(&probe_st.lock);
	if (err)
		snprintf(probe_st.result, sizeof(probe_st.result), "%d\n", err);
	else
		snprintf(probe_st.result, sizeof(probe_st.result),
			 "0 %x %x %x %x %x %x %x %x\n",
			 le32_to_cpu(r.res[0]), le32_to_cpu(r.res[1]),
			 le32_to_cpu(r.res[2]), le32_to_cpu(r.res[3]),
			 le32_to_cpu(r.res[4]), le32_to_cpu(r.res[5]),
			 le32_to_cpu(r.res[6]), le32_to_cpu(r.res[7]));
	mutex_unlock(&probe_st.lock);
	return err ?: len;
}

static ssize_t probe_read(struct file *file, char __user *buf, size_t len,
			  loff_t *ppos)
{
	ssize_t ret;

	mutex_lock(&probe_st.lock);
	ret = simple_read_from_buffer(buf, len, ppos, probe_st.result,
				      strlen(probe_st.result));
	mutex_unlock(&probe_st.lock);
	return ret;
}

static const struct file_operations probe_fops = {
	.open = simple_open,
	.read = probe_read,
	.write = probe_write,
	.llseek = default_llseek,
};

/* round trip of NOP commands: doorbell, NPU, event, wake-up */
static int cmd_bench_show(struct seq_file *s, void *data)
{
	struct libranpu *npu = s->private;
	u64 min = U64_MAX, max = 0, sum = 0, dt;
	u32 i, n = 1000, evt0 = npu->evt_cons;
	ktime_t t0;
	int err;

	for (i = 0; i < n; i++) {
		t0 = ktime_get();
		err = libranpu_cmd(npu, LIBRANPU_SVC_CTL, LIBRANPU_CTL_NOP,
				   NULL, 0, NULL, NULL);
		if (err)
			return err;
		dt = ktime_to_ns(ktime_sub(ktime_get(), t0));
		min = min(min, dt);
		max = max(max, dt);
		sum += dt;
	}
	seq_printf(s, "%u commands: min %llu avg %llu max %llu ns, %u events\n",
		   n, min, div_u64(sum, n), max, npu->evt_cons - evt0);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(cmd_bench);

void libranpu_debugfs_init(struct libranpu *npu)
{
	npu->debugfs = debugfs_create_dir(dev_name(npu->dev), NULL);
	debugfs_create_file("status", 0400, npu->debugfs, npu, &status_fops);
	debugfs_create_file("dbg_block", 0400, npu->debugfs, npu,
			    &dbg_block_fops);
	debugfs_create_file("cmd_bench", 0400, npu->debugfs, npu,
			    &cmd_bench_fops);
	if (le32_to_cpu(npu->caps.services) & LIBRANPU_SVC_F_DBG)
		debugfs_create_file("probe", 0600, npu->debugfs, npu,
				    &probe_fops);
}

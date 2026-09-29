// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * debugfs, bench use only: raw debug block, hart and task state, and a
 * hook to run the firmware's hardware probes.
 */

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/platform_device.h>
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

/*
 * Host adaptor map: for rx ring r and line l, route status bit 16+r to
 * line l, have the NPU write the ring's dma index, count the lines.
 */
#define HA_REG(x)		(0x30d000 + (x))
#define HA_STATUS		HA_REG(0x30)
#define HA_MASK(l)		HA_REG(0x34 + 4 * (l))
#define HA_RX_BASE(r)		HA_REG(0x180 + 0x10 * (r))
#define HA_NPU_ADDR(x)		(0x1ec0d000 + (x))
#define HA_LINES		6
#define HA_FIRST_IRQ		9

struct ha_probe {
	struct libranpu *npu;
	int irq[HA_LINES];
	atomic_t hits[HA_LINES];
	u32 status[HA_LINES];
};

static irqreturn_t ha_probe_irq(int irq, void *data)
{
	struct ha_probe *hp = data;
	int l;

	for (l = 0; l < HA_LINES; l++)
		if (hp->irq[l] == irq)
			break;
	if (l == HA_LINES)
		return IRQ_NONE;
	hp->status[l] = npu_rr(hp->npu, HA_STATUS);
	npu_wr(hp->npu, HA_STATUS, hp->status[l]);
	/* an unacked level line must not storm */
	if (atomic_inc_return(&hp->hits[l]) > 64)
		disable_irq_nosync(irq);
	return IRQ_HANDLED;
}

static int ha_poke(struct libranpu *npu, u32 addr, u32 val)
{
	struct libranpu_dbg_poke p = {
		.addr = cpu_to_le32(addr), .val = cpu_to_le32(val),
	};

	return libranpu_cmd(npu, LIBRANPU_SVC_DBG, LIBRANPU_DBG_POKE,
			    &p, sizeof(p), NULL, NULL);
}

static void ha_probe_one(struct seq_file *s, struct ha_probe *hp, int r,
			 int l, dma_addr_t ring)
{
	struct libranpu *npu = hp->npu;
	u32 hits[HA_LINES], st0, st1;
	int i, err;

	for (i = 0; i < HA_LINES; i++) {
		npu_wr(npu, HA_MASK(i), i == l ? BIT(16 + r) : 0);
		atomic_set(&hp->hits[i], 0);
		hp->status[i] = 0;
	}
	npu_wr(npu, HA_RX_BASE(r), ring);
	npu_wr(npu, HA_RX_BASE(r) + 4, 8);
	npu_wr(npu, HA_RX_BASE(r) + 0xc, 0);
	npu_wr(npu, HA_STATUS, ~0);
	st0 = npu_rr(npu, HA_STATUS);

	err = ha_poke(npu, HA_NPU_ADDR(0x188 + 0x10 * r), 1);
	usleep_range(2000, 3000);
	for (i = 0; i < HA_LINES; i++)
		hits[i] = atomic_read(&hp->hits[i]);
	st1 = npu_rr(npu, HA_STATUS);

	/* the same index again: does a rewrite raise it again */
	ha_poke(npu, HA_NPU_ADDR(0x188 + 0x10 * r), 1);
	usleep_range(2000, 3000);

	seq_printf(s, "rx%d line%d: poke %d, status %08x -> %08x (irq %08x), didx %u, hits",
		   r, l, err, st0, st1, hp->status[l],
		   npu_rr(npu, HA_RX_BASE(r) + 8));
	for (i = 0; i < HA_LINES; i++)
		seq_printf(s, " %u", hits[i]);
	seq_printf(s, ", again %u\n", atomic_read(&hp->hits[l]) - hits[l]);

	npu_wr(npu, HA_MASK(l), 0);
	npu_wr(npu, HA_RX_BASE(r) + 8, 0);
	npu_wr(npu, HA_RX_BASE(r), 0);
	npu_wr(npu, HA_RX_BASE(r) + 4, 0);
}

/* radio 0 counters since its attach, one name and value per line */
static int wlan_stats_show(struct seq_file *s, void *data)
{
	struct libranpu_rxb *b = &((struct libranpu *)s->private)->rxb;
	struct libranpu_wlan_stats st;
	const __le32 *v = (const __le32 *)&st;
	static const char * const names[] = {
		"rx_frames", "rx_ind0", "rx_ind1", "rx_ind2", "rx_ind3",
		"rx_ind4", "rx_ind5", "rx_ind6", "rx_ind7", "rx_ind8",
		"rx_ind9", "rx_ind10", "rx_ind11", "rx_ind12", "rx_ind13",
		"rx_ind14", "rx_ind15", "rx_stale", "rx_pn_fail", "rx_bad_id",
		"host_segs", "host_full", "host_dropped", "buf_refill0",
		"buf_refill1", "buf_returned", "buf_bad_ret", "buf_empty",
		"ppe_tx", "ppe_full", "ppe_bound", "ppe_unbound", "ppe_bad_id",
	};
	int i, err;

	BUILD_BUG_ON(sizeof(names) / sizeof(names[0]) !=
		     offsetof(typeof(st), ppe_crsn) / 4);
	err = libranpu_wlan_stats(s->private, 0, &st);
	if (err)
		return err;
	for (i = 0; i < ARRAY_SIZE(names); i++)
		if (v[i])
			seq_printf(s, "%s %u\n", names[i], le32_to_cpu(v[i]));
	for (i = 0; i < ARRAY_SIZE(st.ppe_crsn); i++)
		if (st.ppe_crsn[i])
			seq_printf(s, "ppe_crsn%02x %u\n", i,
				   le16_to_cpu(st.ppe_crsn[i]));
	/* host side, read without the NAPI context */
	seq_printf(s, "host_lent_frames %llu\nhost_copied_frames %llu\n",
		   b->lent_frames, b->copied_frames);
	seq_printf(s, "host_reclaimed %llu\nhost_lent_now %u of %u\n",
		   b->reclaimed, b->lent, b->lend_max);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(wlan_stats);

/* bench switch: 1 sends every radio 0 frame to the host, not the PPE */
static int wlan_force_host_set(void *data, u64 val)
{
	return libranpu_wlan_force_host(data, 0, !!val);
}
DEFINE_DEBUGFS_ATTRIBUTE(wlan_force_host_fops, NULL, wlan_force_host_set,
			 "%llu\n");

static int ha_probe_show(struct seq_file *s, void *data)
{
	struct libranpu *npu = s->private;
	struct platform_device *pdev = to_platform_device(npu->dev);
	struct ha_probe *hp;
	dma_addr_t ring;
	void *mem;
	int l, r, err = 0;

	hp = kzalloc(sizeof(*hp), GFP_KERNEL);
	mem = dma_alloc_coherent(npu->dev, SZ_4K, &ring, GFP_KERNEL);
	if (!hp || !mem) {
		err = -ENOMEM;
		goto out;
	}
	hp->npu = npu;
	for (l = 0; l < HA_LINES; l++) {
		hp->irq[l] = platform_get_irq_optional(pdev, HA_FIRST_IRQ + l);
		if (hp->irq[l] < 0 ||
		    request_irq(hp->irq[l], ha_probe_irq, 0, "libranpu-ha", hp)) {
			seq_printf(s, "line%d: no irq\n", l);
			hp->irq[l] = -1;
		}
	}
	for (l = 0; l < HA_LINES; l++)
		seq_printf(s, "mask%d %08x\n", l, npu_rr(npu, HA_MASK(l)));

	for (r = 0; r < 4; r++)
		for (l = 0; l < HA_LINES; l++)
			ha_probe_one(s, hp, r, l, ring);

	for (l = 0; l < HA_LINES; l++) {
		npu_wr(npu, HA_MASK(l), 0);
		if (hp->irq[l] >= 0)
			free_irq(hp->irq[l], hp);
	}
out:
	if (mem)
		dma_free_coherent(npu->dev, SZ_4K, mem, ring);
	kfree(hp);
	return err;
}
DEFINE_SHOW_ATTRIBUTE(ha_probe);

void libranpu_debugfs_init(struct libranpu *npu)
{
	npu->debugfs = debugfs_create_dir(dev_name(npu->dev), NULL);
	debugfs_create_file("status", 0400, npu->debugfs, npu, &status_fops);
	debugfs_create_file("dbg_block", 0400, npu->debugfs, npu,
			    &dbg_block_fops);
	debugfs_create_file("wlan_stats", 0400, npu->debugfs, npu,
			    &wlan_stats_fops);
	debugfs_create_file_unsafe("wlan_force_host", 0200, npu->debugfs, npu,
				   &wlan_force_host_fops);
	/* bench switch: 0 copies every rx frame */
	debugfs_create_bool("wlan_rx_lend", 0600, npu->debugfs, &npu->rxb.lend);
	debugfs_create_file("cmd_bench", 0400, npu->debugfs, npu,
			    &cmd_bench_fops);
	if (le32_to_cpu(npu->caps.services) & LIBRANPU_SVC_F_DBG)
		debugfs_create_file("probe", 0600, npu->debugfs, npu,
				    &probe_fops);
	if (le32_to_cpu(npu->caps.services) & LIBRANPU_SVC_F_DBG)
		debugfs_create_file("ha_probe", 0400, npu->debugfs, npu,
				    &ha_probe_fops);
}

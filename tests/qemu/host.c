// SPDX-License-Identifier: GPL-3.0-only
/*
 * Host model on the spare QEMU hart. It does what the kernel driver
 * does: load the image, boot block, trigger, then the command and
 * event rings; and checks every answer.
 */

#include <stdarg.h>
#include "fw/csr.h"
#include "fw/errno.h"
#include "fw/lib.h"
#include "map.h"
#include "plat/regs.h"
#include <linux/soc/airoha/libranpu_abi.h>

#define IMG_ADDR	0x8A000000
#define BOOT_ADDR	0x89000000
#define CMD_ADDR	(BOOT_ADDR + 0x1000)
#define EVT_ADDR	(CMD_ADDR + LIBRANPU_CMD_ENTRIES * LIBRANPU_CMD_SIZE)
#define SIFIVE_TEST	0x00100000
#define HARTS		6
/* QEMU mcycle runs near the host clock; generous bounds */
#define TIMEOUT		4000000000u

static volatile struct libranpu_boot *const bb = (void *)BOOT_ADDR;
static volatile struct libranpu_cmd *const cmdr = (void *)CMD_ADDR;
static volatile struct libranpu_evt *const evtr = (void *)EVT_ADDR;

static u32 cmd_prod, evt_cons, doorbell, seq = 100;
static u32 slot_seq[LIBRANPU_CMD_ENTRIES];	/* 0: free */
static int failures, checks;

static void out(const char *fmt, ...)
{
	char buf[160];
	va_list ap;
	int i;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	for (i = 0; buf[i]; i++) {
		while (!(*(volatile u8 *)(QEMU_UART + 5) & 0x20))
			;
		*(volatile u8 *)QEMU_UART = buf[i];
	}
}

#define CHECK(c, ...) do {						\
	checks++;							\
	if (!(c)) {							\
		failures++;						\
		out("FAIL %s:%d: %s: ", __func__, __LINE__, #c);	\
		out(__VA_ARGS__);					\
		out("\n");						\
	}								\
} while (0)

static void __noreturn finish(void)
{
	out("%s: %d checks, %d failed\n", failures ? "FAIL" : "PASS", checks,
	    failures);
	REG32(SIFIVE_TEST) = failures ? (1 << 16) | 0x3333 : 0x5555;
	for (;;)
		;
}

static u32 crc32_add(u32 c, const u8 *p, u32 n)
{
	u32 k;

	while (n--) {
		c ^= *p++;
		for (k = 0; k < 8; k++)
			c = c >> 1 ^ (0xedb88320 & -(c & 1));
	}
	return c;
}

/* ---- load and boot, as the driver probe does -------------------- */

static int load_image(u32 *entry, u32 *harts)
{
	const struct libranpu_img_hdr *h = (const void *)IMG_ADDR;
	u32 len = h->data.offset + h->data.size, crc;

	CHECK(h->magic == LIBRANPU_IMG_MAGIC, "magic %x", h->magic);
	CHECK(h->hdr_size == LIBRANPU_IMG_HDR_SIZE, "hdr %u", h->hdr_size);
	CHECK(h->soc == LIBRANPU_SOC_AN7583, "soc %x", h->soc);
	CHECK(h->abi_major == LIBRANPU_ABI_MAJOR, "abi %u", h->abi_major);
	CHECK(h->code.offset == LIBRANPU_IMG_HDR_SIZE, "code at %u",
	      h->code.offset);
	CHECK(h->cluster_size <= h->dbg_offset, "cluster %u", h->cluster_size);
	CHECK(h->services & LIBRANPU_SVC_F_DBG, "services %x", h->services);
	if (failures)
		return -EINVAL;

	/* crc over the header without its own field, and the sections */
	crc = crc32_add(~0u, (const u8 *)h, 124);
	crc = ~crc32_add(crc, (const u8 *)h + 128, len - 128);
	CHECK(crc == h->crc32, "crc %x vs %x", crc, h->crc32);

	memcpy((void *)h->code.load, (const u8 *)h + h->code.offset,
	       h->code.size);
	memcpy((void *)h->data.load, (const u8 *)h + h->data.offset,
	       h->data.size);
	*entry = h->entry;
	*harts = h->harts;
	return 0;
}

static void boot(void)
{
	u32 entry, harts, t0, h;

	if (load_image(&entry, &harts))
		finish();

	memset((void *)BOOT_ADDR, 0, 0x1000 + 2 * 0x2000);
	bb->magic = LIBRANPU_BOOT_MAGIC;
	bb->abi_major = LIBRANPU_ABI_MAJOR;
	bb->abi_minor = LIBRANPU_ABI_MINOR;
	bb->flags = LIBRANPU_BOOT_F_UART;
	bb->cmd_ring = CMD_ADDR;
	bb->evt_ring = EVT_ADDR;
	bb->cmd_entries = LIBRANPU_CMD_ENTRIES;
	bb->evt_entries = LIBRANPU_EVT_ENTRIES;
	mb();

	REG32(REG_MIB(MIB_BOOT_BLOCK)) = BOOT_ADDR;
	for (h = 0; h < harts; h++)
		REG32(REG_BOOT_BASE(h)) = entry;
	REG32(REG_BOOT_CONFIG) = BIT(harts) - 1;
	mb();
	REG32(REG_BOOT_TRIGGER) = 1;

	/* host memory only until ready */
	for (t0 = cycles(); bb->ready != LIBRANPU_BOOT_READY;)
		if (cycles() - t0 > TIMEOUT) {
			CHECK(0, "no ready");
			finish();
		}
	rmb();
	out("ready: status %d harts %x boot %u cycles, host saw %u\n",
	    (int)bb->status, bb->harts_up, bb->boot_cycles, cycles() - t0);
	CHECK(bb->status == 0, "status %d", (int)bb->status);
	CHECK(bb->harts_up == BIT(HARTS) - 1, "harts %x", bb->harts_up);
	CHECK(bb->fw_version != 0, "version");
}

/* ---- rings ------------------------------------------------------ */

struct done {
	u32 seq;
	s32 status;
	u16 opcode;
	u8 service;
};

static struct done dones[64];
static u32 ndone, events[LIBRANPU_EVT_MAX], bad_events;

static void drain(void)
{
	u32 prod;

	for (;;) {
		prod = bb->evt_prod;
		rmb();
		if (prod - evt_cons > LIBRANPU_EVT_ENTRIES) {
			bad_events++;
			return;
		}
		while (evt_cons != prod) {
			volatile struct libranpu_evt *e =
				&evtr[evt_cons & (LIBRANPU_EVT_ENTRIES - 1)];
			u16 type = e->type;

			if (type >= LIBRANPU_EVT_MAX ||
			    e->len > LIBRANPU_EVT_PAYLOAD) {
				bad_events++;
			} else {
				events[type]++;
			}
			if (type == LIBRANPU_EVT_CMD_DONE &&
			    ndone < ARRAY_SIZE(dones)) {
				const volatile struct libranpu_evt_cmd_done *d =
					(const volatile void *)e->payload;

				dones[ndone].seq = e->seq;
				dones[ndone].status = d->status;
				dones[ndone].opcode = d->opcode;
				dones[ndone].service = d->service;
				ndone++;
			}
			evt_cons++;
		}
		bb->evt_cons = evt_cons;
		mb();
		if (bb->evt_prod == prod)
			return;
	}
}

static bool take_done(u32 s, s32 *status)
{
	u32 i;

	for (i = 0; i < ndone; i++) {
		if (dones[i].seq != s)
			continue;
		*status = dones[i].status;
		dones[i] = dones[--ndone];
		return true;
	}
	return false;
}

static u32 cmd_send(u8 svc, u16 op, const void *req, u16 len, u8 flags)
{
	u32 slot = cmd_prod & (LIBRANPU_CMD_ENTRIES - 1), t0;
	volatile struct libranpu_cmd *c = &cmdr[slot];

	/* a slot is reused only once its completion came in */
	for (t0 = cycles(); slot_seq[slot];) {
		s32 st;

		drain();
		if (take_done(slot_seq[slot], &st))
			slot_seq[slot] = 0;
		if (cycles() - t0 > TIMEOUT) {
			CHECK(0, "slot %u stuck", slot);
			finish();
		}
	}

	c->opcode = op;
	c->service = svc;
	c->flags = flags;
	c->seq = ++seq;
	c->len = len;
	c->rsp_len = 0;
	c->status = 0x7fffffff;
	copy_words((volatile u32 *)c->payload, req, len);
	slot_seq[slot] = flags & LIBRANPU_CMD_F_NO_EVENT ? 0 : seq;
	wmb();
	bb->cmd_prod = ++cmd_prod;
	wmb();
	REG32(REG_MBQ_CTRL(0, 2)) = ++doorbell;
	return seq;
}

static s32 cmd_wait(u32 s, void *rsp, u16 *rsp_len)
{
	u32 slot, t0;
	s32 status;

	for (t0 = cycles(); !take_done(s, &status); drain())
		if (cycles() - t0 > TIMEOUT) {
			CHECK(0, "seq %u no answer", s);
			finish();
		}

	for (slot = 0; slot < LIBRANPU_CMD_ENTRIES; slot++)
		if (slot_seq[slot] == s)
			break;
	CHECK(slot < LIBRANPU_CMD_ENTRIES, "seq %u no slot", s);
	slot_seq[slot] = 0;
	CHECK(cmdr[slot].status == (u32)status, "slot status %d vs %d",
	      (int)cmdr[slot].status, (int)status);
	if (rsp_len)
		*rsp_len = cmdr[slot].rsp_len;
	if (rsp && !status)
		copy_words(rsp, (volatile u32 *)cmdr[slot].payload,
			   cmdr[slot].rsp_len);
	return status;
}

static s32 cmd(u8 svc, u16 op, const void *req, u16 len, void *rsp,
	       u16 *rsp_len)
{
	return cmd_wait(cmd_send(svc, op, req, len, 0), rsp, rsp_len);
}

/* ---- tests ------------------------------------------------------ */

static void test_ctl(void)
{
	u32 req[16], rsp[60], i;
	struct libranpu_caps caps;
	u16 len;
	s32 st;

	for (i = 0; i < 16; i++)
		req[i] = 0x1000 + i;
	st = cmd(LIBRANPU_SVC_CTL, LIBRANPU_CTL_NOP, req, 64, rsp, &len);
	CHECK(st == 0 && len == 64 && !memcmp(req, rsp, 64), "nop %d %u",
	      (int)st, len);

	st = cmd(LIBRANPU_SVC_CTL, LIBRANPU_CTL_GET_CAPS, NULL, 0, &caps,
		 &len);
	CHECK(st == 0 && len == sizeof(caps), "caps %d %u", (int)st, len);
	CHECK(caps.abi_major == LIBRANPU_ABI_MAJOR, "abi");
	CHECK(caps.soc == LIBRANPU_SOC_AN7583 && caps.harts == HARTS,
	      "soc %x harts %u", caps.soc, caps.harts);
	CHECK(caps.harts_up == BIT(HARTS) - 1, "up %x", caps.harts_up);
	CHECK(caps.sram_total == 0x80000, "sram %u", caps.sram_total);
	CHECK(caps.sram_free < caps.sram_total, "free %u", caps.sram_free);
	CHECK(caps.services & LIBRANPU_SVC_F_DBG, "svc %x", caps.services);
	CHECK(caps.task_map[0] & BIT(LIBRANPU_TASK_CTL), "map %x",
	      caps.task_map[0]);
	for (i = 1; i < HARTS; i++)
		CHECK(caps.task_map[i] == BIT(LIBRANPU_TASK_DBG),
		      "map%u %x", i, caps.task_map[i]);
	out("caps: fw %x cpu %u MHz sram %u/%u cluster %u/%u\n",
	    caps.fw_version, caps.cpu_mhz, caps.sram_free, caps.sram_total,
	    caps.cluster_free, caps.cluster_total);

	st = cmd(LIBRANPU_SVC_CTL, LIBRANPU_CTL_GET_TASKS, NULL, 0, rsp,
		 &len);
	CHECK(st == 0 && len == 8 * sizeof(struct libranpu_task_info),
	      "tasks %d %u", (int)st, len);

	/* errors */
	CHECK(cmd(LIBRANPU_SVC_CTL, 0x7777, NULL, 0, NULL, NULL) ==
	      -EOPNOTSUPP, "opcode");
	CHECK(cmd(9, 0, NULL, 0, NULL, NULL) == -EOPNOTSUPP, "service");
	CHECK(cmd(LIBRANPU_SVC_CTL, LIBRANPU_CTL_STATS_CONFIG, NULL, 0,
		  NULL, NULL) == -EOPNOTSUPP, "stats config");
	CHECK(cmd(LIBRANPU_SVC_DBG, LIBRANPU_DBG_PEEK, req, 4, NULL,
		  NULL) == -EINVAL, "short payload");
}

/* more commands than slots: slots come back in order */
static void test_burst(void)
{
	u32 s[3 * LIBRANPU_CMD_ENTRIES], i, v, rsp[60];
	u16 len;

	for (i = 0; i < ARRAY_SIZE(s); i++) {
		v = i;
		s[i] = cmd_send(LIBRANPU_SVC_CTL, LIBRANPU_CTL_NOP, &v, 4, 0);
		if (i >= LIBRANPU_CMD_ENTRIES - 1) {
			u32 j = i - (LIBRANPU_CMD_ENTRIES - 1);

			CHECK(cmd_wait(s[j], rsp, &len) == 0 && len == 4 &&
			      rsp[0] == j, "burst %u", j);
		}
	}
	for (i = ARRAY_SIZE(s) - (LIBRANPU_CMD_ENTRIES - 1);
	     i < ARRAY_SIZE(s); i++)
		CHECK(cmd_wait(s[i], rsp, &len) == 0 && rsp[0] == i, "tail %u",
		      i);
	CHECK(!bad_events, "bad events %u", bad_events);
	out("burst: %u commands, %u host interrupts\n", (u32)ARRAY_SIZE(s),
	    REG32(REG_MBQ_CTRL(MBQ_TO_HOST, 2)));
}

static s32 probe(u8 hart, u8 id, u32 a0, u32 a1, u32 a2,
		 struct libranpu_dbg_probe_rsp *r)
{
	struct libranpu_dbg_probe p = {
		.hart = hart, .probe = id, .arg = { a0, a1, a2, 0 },
	};

	memset(r, 0, sizeof(*r));
	return cmd(LIBRANPU_SVC_DBG, LIBRANPU_DBG_PROBE, &p, sizeof(p), r,
		   NULL);
}

static void test_dbg(void)
{
	struct libranpu_dbg_peek pk = { NPU_CLUSTER_BASE + 0x6800, 4 };
	struct libranpu_dbg_poke po = { 0x89008000, 0xfeedf00d };
	struct libranpu_dbg_probe_rsp r;
	u32 rsp[60], h;
	u16 len;
	s32 st;

	st = cmd(LIBRANPU_SVC_DBG, LIBRANPU_DBG_PEEK, &pk, sizeof(pk), rsp,
		 &len);
	CHECK(st == 0 && len == 16 && rsp[0] == LIBRANPU_DBG_MAGIC,
	      "peek %d %x", (int)st, rsp[0]);

	st = cmd(LIBRANPU_SVC_DBG, LIBRANPU_DBG_POKE, &po, sizeof(po),
		 NULL, NULL);
	CHECK(st == 0 && REG32(0x89008000) == 0xfeedf00d, "poke %d", (int)st);

	pk.addr = 0xf0000000;
	pk.words = 1;
	st = cmd(LIBRANPU_SVC_DBG, LIBRANPU_DBG_PEEK, &pk, sizeof(pk), rsp,
		 NULL);
	CHECK(st == -EFAULT, "peek fault %d", (int)st);
	pk.words = 61;
	CHECK(cmd(LIBRANPU_SVC_DBG, LIBRANPU_DBG_PEEK, &pk, sizeof(pk),
		  NULL, NULL) == -EINVAL, "peek size");

	for (h = 0; h < HARTS; h++) {
		st = probe(h, LIBRANPU_PROBE_CYCLES, 1000, 0, 0, &r);
		CHECK(st == 0 && r.res[0] && r.res[2] == 1000,
		      "cycles hart %u: %d %u", h, (int)st, r.res[0]);
	}

	st = probe(3, LIBRANPU_PROBE_LOAD, 0x89008000, 64, 64, &r);
	CHECK(st == 0 && r.res[0] && r.res[1] && !r.res[2], "load %d", (int)st);
	st = probe(3, LIBRANPU_PROBE_LOAD, 0xf0000000, 4, 2, &r);
	CHECK(st == 0 && r.res[2], "load fault %d %x", (int)st, r.res[2]);

	st = probe(1, LIBRANPU_PROBE_CLUSTER, 0, 0, 0, &r);
	CHECK(st == 0, "cluster %d", (int)st);
	out("cluster: size %x kind %u cause %x\n", r.res[0], r.res[1],
	    r.res[2]);

	st = probe(2, LIBRANPU_PROBE_WFI, 1000000, 0, 0, &r);
	CHECK(st == 0 && r.res[4] == 1 && r.res[3], "wfi %d ack %u src %u",
	      (int)st, r.res[4], r.res[3]);
	out("wfi: slept %u cycles, raised after %u, mip %x/%x\n", r.res[0],
	    r.res[5], r.res[1], r.res[2]);

	st = probe(4, LIBRANPU_PROBE_XHART, 0, 0, 0, &r);
	CHECK(st == 0 && r.res[0] == r.res[6] && r.res[4] == r.res[7],
	      "xhart %d", (int)st);
	out("xhart: saw %x, hart0 cached %x dram %x, after wb %x cached %x\n",
	    r.res[0], r.res[2], r.res[3], r.res[4], r.res[5]);

	CHECK(probe(0, LIBRANPU_PROBE_WFI, 0, 0, 0, &r) == -EINVAL, "wfi 0");
	CHECK(probe(7, LIBRANPU_PROBE_CYCLES, 0, 0, 0, &r) == -EINVAL,
	      "hart 7");
}

static void test_reset(void)
{
	struct libranpu_reset_rsp r;
	const volatile struct libranpu_dbg_block *d = (const void *)(NPU_CLUSTER_BASE +
							       0x6800);
	u32 h;
	s32 st;

	st = cmd(LIBRANPU_SVC_CTL, LIBRANPU_CTL_RESET, NULL, 0, &r, NULL);
	CHECK(st == 0 && r.parked == BIT(HARTS) - 1, "reset %d %x", (int)st,
	      r.parked);
	for (h = 0; h < 10000000; h++)
		if (d->hart[0].state == LIBRANPU_HART_PARKED)
			break;
	for (h = 0; h < HARTS; h++)
		CHECK(d->hart[h].state == LIBRANPU_HART_PARKED, "hart %u %u",
		      h, d->hart[h].state);
	for (h = 0; h < HARTS; h++)
		CHECK(!bb->fault[h], "fault %u %x", h, bb->fault[h]);
}

void host_main(void)
{
	out("host: an7583 image at %x\n", IMG_ADDR);
	boot();
	test_ctl();
	test_burst();
	test_dbg();
	test_reset();
	finish();
}

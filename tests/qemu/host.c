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
		CHECK(caps.task_map[i] & BIT(LIBRANPU_TASK_DBG),
		      "map%u %x", i, caps.task_map[i]);
	CHECK(caps.task_map[1] & BIT(LIBRANPU_TASK_RX), "rx map");
	CHECK(caps.task_map[4] & BIT(LIBRANPU_TASK_BUF), "buf map");
	CHECK(caps.wlan_backends & LIBRANPU_WLAN_BE_RRO31, "backends");
	out("caps: fw %x cpu %u MHz sram %u/%u cluster %u/%u\n",
	    caps.fw_version, caps.cpu_mhz, caps.sram_free, caps.sram_total,
	    caps.cluster_free, caps.cluster_total);

	st = cmd(LIBRANPU_SVC_CTL, LIBRANPU_CTL_GET_TASKS, NULL, 0, rsp,
		 &len);
	CHECK(st == 0 && len == 12 * sizeof(struct libranpu_task_info),
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

/* ---- WLAN: a chip model and the host driver's side ------------- */

#define POOL_BASE	0x8B000000
#define POOL_IDS	2048
#define CHIP_REGS	0x89010000	/* fake chip ring register blocks */
#define HRX_BASE	0x89020000
#define HRET_BASE	0x89030000
#define HELD_BASE	0x89038000	/* ids held from a previous attach */
#define HTX_BASE	0x89060000	/* host tx descriptors, per band */
#define HTX_ENTRIES	256
#define TX_ENTRIES	128
#define TX_DIDX0	5		/* chip dma index before the start */
#define TX_CTRL		0x004C4048	/* 76-byte TXWI, 72-byte head */
#define TXF_ENTRIES	64		/* chip tx free rings */
#define HTXF_BASE	0x89070000	/* tx free records to the host */
#define TXFD_BASE	0x89072000	/* host tx free rings, per band */
#define TXFB_BASE	0x8C000000	/* their 2 KB buffers */
#define TXF_BUF64	27		/* 1728 bytes armed */
#define TXF_DIDX0	3
#define LAN_POOL	0x8C800000	/* NPU token buffers */
#define LAN_TOKENS	2150	/* 2048 parked in the rings: few left */
#define LAN_RING	1024
#define TDMA_RX(k)	(NPU_FE_BASE + 0x900 + 0x10 * (k))
#define HTXF_ENTRIES	256
#define TXF_BATCH	24		/* tokens per chip report */
#define HEADROOM	64
#define BUF_LEN		1664
#define RX_ENTRIES	256
#define RXD_ENTRIES	512
#define HRX_ENTRIES	512
#define HRET_ENTRIES	1024
#define HA_RX(n)	(NPU_MMIO_BASE + 0xD180 + 0x10 * (n))
#define HA_TX(n)	(NPU_MMIO_BASE + 0xD080 + 0x10 * (n))

struct chip {
	u32 tx_desc[2];
	u32 tx_didx[2];
	u32 tx_taken, tx_bad;
	u32 txf_desc[2], txf_idx[2];
	u16 pend[2][TXF_BATCH];
	u32 npend[2], reports[2];
	u32 other_sent[2], others;
	u32 lan_taken, lan_bad, txf_hold;
	u32 rx_desc[2];
	u32 rxd_desc;
	u32 rx_idx[2];
	u32 rxd_idx;
	u32 rxd_gen;
	u32 sent;
	u32 stale;
	u32 gap;			/* big sequence gap: a good frame */
};

static struct chip chip;
static u32 hrx_cons, hret_prod, host_got, host_bad, chip_held;
static u32 htx_prod[2], htx_seq[2];
static u32 htxf_cons, htxf_tok[2], htxf_stat[2], htxf_bad, htxf_other;
static u32 lan_didx[2], lan_sent, lan_seq;

/* frame engine: n frames of bound flows on TDMA rx ring k (= band) */
static void fe_lan(u32 k, u32 n)
{
	u32 ring = REG32(TDMA_RX(k)) | 0x80000000, cpu = REG32(TDMA_RX(k) + 8);

	while (n-- && lan_didx[k] != cpu) {
		volatile u32 *d = (u32 *)(ring + 32 * lan_didx[k]);
		u32 len = 60 + lan_seq % 1400, i;
		u8 *buf = (u8 *)d[2];

		if (d[1] & BIT(31))
			break;
		for (i = 0; i < len; i++)
			buf[i] = (u8)(lan_seq + i);
		d[4] = k << 25 | (100 + k) << 14;
		d[6] = (u32)(7 + k) << 24;
		wmb();
		d[1] = BIT(31) | len;
		lan_didx[k] = (lan_didx[k] + 1) % LAN_RING;
		REG32(TDMA_RX(k) + 0xC) = lan_didx[k];
		lan_seq++;
		lan_sent++;
	}
}

/* the chip's view of a LAN frame: zero TXD, TXP, frame at +128 */
static bool chip_lan_ok(u32 b, volatile u32 *d)
{
	u32 tok = (d[0] - LAN_POOL) / 2048, i;
	volatile u32 *txd = (u32 *)d[0];
	const u8 *f = (u8 *)d[2];

	for (i = 0; i < 8; i++)
		if (txd[i])
			return false;
	if (d[2] != d[0] + 128 || txd[8] != (0x80 | tok << 16) ||
	    txd[9] != ((7 + b) | (100 + b) << 8 | BIT(24)) ||
	    txd[10] != d[2] || txd[16] < 60 || txd[16] > 1920)
		return false;
	for (i = 1; i < txd[16]; i++)
		if ((u8)(f[i] - f[0]) != (u8)i)
			return false;
	return true;
}
static bool htxf_hold;
static volatile u32 *const held_map = (u32 *)HELD_BASE;

static bool held(u32 id)
{
	return held_map[id / 32] & BIT(id % 32);
}

/*
 * Frame engine model: takes the TDMA tx ring, binds one id in three,
 * shows the rest on the buffer FIFO head until the firmware pops it.
 */
#define FE(off)		REG32(NPU_FE_BASE + (off))
#define FEQ		4096

static struct {
	u32 dtx;
	u32 q[FEQ], inf[FEQ];
	u32 qh, qt;
	u32 bound, unbound, bad;
	u8 hdr[POOL_IDS];		/* per id: 802.3 offset the chip set */
	u16 sdl[POOL_IDS];
} fe;

static void fe_step(void)
{
	u32 ctx = FE(0x808) % 1024, ring = FE(0x800) | 0x80000000;

	while (fe.dtx != ctx && fe.qt - fe.qh < FEQ) {
		volatile u32 *d = (u32 *)(ring + 8 * fe.dtx);
		u32 w0 = d[0], id = (w0 >> 14) & 0x7FFF, len = w0 & 0x1FFF;
		u32 want = fe.sdl[id] - 2 * fe.hdr[id], i;
		const u8 *b = (u8 *)d[1];

		if (id >= POOL_IDS || !(w0 & BIT(30)) ||
		    d[1] != POOL_BASE + id * 2048 + HEADROOM + 2 * fe.hdr[id] ||
		    len != (want < 60 ? 60 : want)) {
			fe.bad++;
		} else {
			for (i = 1; i < len; i++)
				if (i < want ? (u8)(b[i] - b[0]) != (u8)i : b[i]) {
					fe.bad++;
					break;
				}
		}
		fe.q[fe.qt % FEQ] = BIT(31) | (id % 3 ? 0 : BIT(30)) |
				    0x2A << 16 | id;
		fe.inf[fe.qt % FEQ] = 0x0F << 16 | ((id * 7) & 0x7FFF);
		fe.qt++;
		fe.dtx = (fe.dtx + 1) % 1024;
		FE(0x80C) = fe.dtx;
	}
	/* the head is ours again once popped (cleared) */
	if (!FE(0xFE0)) {
		if (fe.qh == fe.qt) {
			FE(0xFE0) = 0;
			return;
		}
		if (fe.q[fe.qh % FEQ] & BIT(30))
			fe.bound++;
		else
			fe.unbound++;
		FE(0xFDC) = fe.inf[fe.qh % FEQ];
		wmb();
		FE(0xFE0) = fe.q[fe.qh % FEQ];
		fe.qh++;
	}
}

static u32 regs_of(int i)
{
	return CHIP_REGS + 0x10 * i;
}

/* chip tx: take slots up to the cpu index, check them, set done */
/* a version 5 report: pair, header, then two tokens a word */
static void chip_txfree(u32 b, bool full)
{
	volatile u32 *d = (u32 *)(chip.txf_desc[b] + 16 * chip.txf_idx[b]);
	u32 n = chip.npend[b], len = 16 + 4 * ((n + 1) / 2), i;
	volatile u32 *ev;

	if (!n || (!full && n < TXF_BATCH / 2) || chip.txf_hold ||
	    chip.txf_idx[b] == REG32(regs_of(5 + b) + 8))
		return;
	if ((d[1] & BIT(31)) || !d[0]) {
		chip.tx_bad++;
		return;
	}
	if (chip.reports[b] % 4 == 3 && !chip.other_sent[b]) {
		ev = (u32 *)d[0];
		for (i = 0; i < 11; i++)
			ev[i] = i ? 0xA0000000 | b << 16 | chip.reports[b] << 4 | i :
				3u << 27 | 44;
		wmb();
		d[1] = 44 << 16 | BIT(30) | BIT(31);
		chip.txf_idx[b] = (chip.txf_idx[b] + 1) % TXF_ENTRIES;
		REG32(regs_of(5 + b) + 0xC) = chip.txf_idx[b];
		chip.other_sent[b] = 1;
		chip.others++;
		return;
	}
	chip.other_sent[b] = 0;
	ev = (u32 *)d[0];
	if ((d[1] >> 16 & 0x3FFF) != TXF_BUF64 * 64)
		chip.tx_bad++;
	ev[0] = 6u << 27 | n << 16 | len;
	ev[1] = 5 << 16;
	ev[2] = BIT(31) | (5 + b) << 12;
	ev[3] = BIT(30) | 2 << 24 | b << 28;
	for (i = 0; i < n; i += 2)
		ev[4 + i / 2] = chip.pend[b][i] |
				(i + 1 < n ? chip.pend[b][i + 1] : 0x7FFFu) << 15;
	wmb();
	d[1] = len << 16 | BIT(30) | BIT(31);
	chip.txf_idx[b] = (chip.txf_idx[b] + 1) % TXF_ENTRIES;
	REG32(regs_of(5 + b) + 0xC) = chip.txf_idx[b];
	chip.npend[b] = 0;
	chip.reports[b]++;
}

/* the driver's tx free poll: tokens in order per band, a status each */
static void host_txfree_poll(void)
{
	volatile u32 *ring = (u32 *)HTXF_BASE;
	u32 prod = REG32(HA_RX(2) + 8);

	if (htxf_hold)
		return;
	while (htxf_cons != prod) {
		u32 w0 = ring[2 * htxf_cons], w1 = ring[2 * htxf_cons + 1];
		u32 wcid = w0 >> 16, b = wcid - 5;

		if ((w1 & 0xFF) == LIBRANPU_TXFREE_EVENT) {
			u32 len = w0 & 0xFFFF, n = (len + 7) / 8, i, v;

			/* the report's own words follow, in order */
			for (i = 0; i < 2 * n && i < len / 4; i++) {
				v = ring[2 * ((htxf_cons + 1 + i / 2) % HTXF_ENTRIES) +
					 i % 2];
				if (i ? (v & 0xFFFF000F) != (0xA0000000 | wcid << 16 | i) :
				    v != (3u << 27 | 44))
					htxf_bad++;
			}
			if (len != 44 || wcid > 1)
				htxf_bad++;
			htxf_other++;
			htxf_cons = (htxf_cons + 1 + n) % HTXF_ENTRIES;
			continue;
		}
		if (b > 1)
			htxf_bad++;
		else if ((w1 & 0xFF) == LIBRANPU_TXFREE_TOKEN) {
			if ((w0 & 0xFFFF) != LAN_TOKENS + htxf_tok[b])
				htxf_bad++;
			htxf_tok[b]++;
		} else if (w1 != (LIBRANPU_TXFREE_STATUS | 2 << 8 | b << 16)) {
			htxf_bad++;
		} else {
			htxf_stat[b]++;
		}
		htxf_cons = (htxf_cons + 1) % HTXF_ENTRIES;
	}
	REG32(HA_RX(2) + 0xC) = htxf_cons;
}

static void chip_tx_step(void)
{
	u32 b;

	for (b = 0; b < 2; b++) {
		u32 cidx = REG32(regs_of(3 + b) + 8);

		while (chip.tx_didx[b] != cidx) {
			volatile u32 *d = (u32 *)(chip.tx_desc[b] +
						  16 * chip.tx_didx[b]);

			/* no report slot: the chip stops taking frames */
			if (chip.npend[b] == TXF_BATCH)
				break;
			if (d[0] >= LAN_POOL && d[0] < LAN_POOL + LAN_TOKENS * 2048) {
				if (d[1] != TX_CTRL || !chip_lan_ok(b, d))
					chip.lan_bad++;
				chip.pend[b][chip.npend[b]++] = (d[0] - LAN_POOL) / 2048;
				chip.lan_taken++;
				d[1] |= BIT(31);
				chip.tx_didx[b] = (chip.tx_didx[b] + 1) % TX_ENTRIES;
				REG32(regs_of(3 + b) + 0xC) = chip.tx_didx[b];
				chip_txfree(b, chip.npend[b] == TXF_BATCH);
				continue;
			}
			if ((d[1] & BIT(31)) || d[1] != TX_CTRL ||
			    (d[0] >> 24) != 0x10 + b || d[2] != d[0] * 2 ||
			    d[3] != (d[0] & 0xFFFFFF))
				chip.tx_bad++;
			d[1] |= BIT(31);
			chip.pend[b][chip.npend[b]++] = LAN_TOKENS + (d[0] & 0x7FFF);
			chip_txfree(b, chip.npend[b] == TXF_BATCH);
			chip.tx_taken++;
			chip.tx_didx[b] = (chip.tx_didx[b] + 1) % TX_ENTRIES;
			REG32(regs_of(3 + b) + 0xC) = chip.tx_didx[b];
		}
	}
}

/* the driver: n descriptors on band b, one index write */
static u32 host_tx(u32 b, u32 n)
{
	volatile u32 *ring = (u32 *)(HTX_BASE + 0x4000 * b);
	u32 cons = REG32(HA_TX(5 + b) + 0xC), i;

	for (i = 0; i < n; i++) {
		volatile u32 *e = ring + 4 * htx_prod[b];
		u32 v;

		if ((htx_prod[b] + 1) % HTX_ENTRIES == cons)
			break;
		v = (0x10 + b) << 24 | htx_seq[b]++;
		e[0] = v;
		e[1] = TX_CTRL;
		e[2] = v * 2;
		e[3] = v & 0xFFFFFF;
		htx_prod[b] = (htx_prod[b] + 1) % HTX_ENTRIES;
	}
	wmb();
	REG32(HA_TX(5 + b) + 8) = htx_prod[b];
	return i;
}

/* completions the NPU has not read yet, from its published cpu index */
static u32 chip_rxd_used(void)
{
	u32 cidx = REG32(regs_of(2) + 8);

	return (chip.rxd_idx + RXD_ENTRIES - cidx - 1) % RXD_ENTRIES;
}

/* one segment: take the band's next slot, fill it, post a completion */
static int chip_rx(u32 band, u32 len, bool last, u32 tag, u32 ind)
{
	volatile u32 *d = (u32 *)(chip.rx_desc[band] + 16 * chip.rx_idx[band]);
	volatile u32 *c = (u32 *)(chip.rxd_desc + 16 * chip.rxd_idx);
	u32 id, i;
	u8 *buf;

	if (d[1] & BIT(31) || chip_rxd_used() >= RXD_ENTRIES - 8)
		return -1;
	id = d[2] >> 16;
	/* some single frames carry 8 bytes before their 802.3 header */
	fe.sdl[id] = len;
	fe.hdr[id] = last && tag % 4 == 1 ? 4 : 0;
	buf = (u8 *)d[0];
	CHECK(d[0] == POOL_BASE + id * 2048 + HEADROOM &&
	      (d[1] >> 16 & 0x3FFF) == BUF_LEN, "rx slot %x %x id %u", d[0],
	      d[1], id);
	chip_held += held(id);
	for (i = 0; i < len; i++)
		buf[i] = (u8)(tag + i);
	d[1] |= BIT(31);
	c[1] = len << 16 | (last ? BIT(30) : 0) | 1 << 11 | fe.hdr[id];
	c[2] = id << 16 | ind << 12;
	wmb();
	c[3] = chip.rxd_gen << 28;
	chip.rx_idx[band] = (chip.rx_idx[band] + 1) % RX_ENTRIES;
	if (++chip.rxd_idx == RXD_ENTRIES) {
		chip.rxd_idx = 0;
		chip.rxd_gen = (chip.rxd_gen + 1) & 0xf;
	}
	chip.sent++;
	chip.stale += ind == 1 || ind == 2;
	chip.gap += ind == 6;
	return 0;
}

/* the driver's rx poll: check, return the ids, publish once */
static u32 host_rx_poll(u32 *frames_seen)
{
	volatile struct libranpu_host_rx *ring = (void *)HRX_BASE;
	volatile u32 *ret = (u32 *)HRET_BASE;
	u32 n = 0, prod;

	fe_step();
	prod = REG32(HA_RX(0) + 8);

	while (hrx_cons != prod) {
		volatile struct libranpu_host_rx *e = &ring[hrx_cons];
		u32 ctrl = e->ctrl, segs = FIELD_GET(LIBRANPU_HRX_SEGS, e->info);
		u32 id = FIELD_GET(LIBRANPU_HRX_ID, e->buf), i;
		u32 len = FIELD_GET(LIBRANPU_HRX_SEG_LEN, ctrl);
		const u8 *b = (u8 *)(POOL_BASE + id * 2048 +
				     FIELD_GET(LIBRANPU_HRX_OFFSET, e->buf));

		if (!(ctrl & LIBRANPU_HRX_DONE) || id >= POOL_IDS) {
			host_bad++;
		} else {
			for (i = 1; i < len; i++)
				if ((u8)(b[i] - b[0]) != (u8)i) {
					host_bad++;
					break;
				}
			if (ctrl & LIBRANPU_HRX_LAST)
				(*frames_seen)++;
			if ((segs != 1 && segs != 3) ||
			    FIELD_GET(LIBRANPU_HRX_REASON, e->info) ==
			    LIBRANPU_HRX_ERROR)
				host_bad++;
			/* back from the PPE: its entry and reason */
			if (FIELD_GET(LIBRANPU_HRX_REASON, e->info) ==
			    LIBRANPU_HRX_PPE &&
			    (FIELD_GET(LIBRANPU_HRX_FOE, e->info) !=
			     ((id * 7) & 0x7FFF) ||
			     FIELD_GET(LIBRANPU_HRX_CRSN, e->info) != 0x0F ||
			     FIELD_GET(LIBRANPU_HRX_OFFSET, e->buf) != HEADROOM))
				host_bad++;
		}
		e->ctrl = 0;
		ret[hret_prod] = id;
		hret_prod = (hret_prod + 1) % HRET_ENTRIES;
		hrx_cons = (hrx_cons + 1) % HRX_ENTRIES;
		host_got++;
		n++;
	}
	if (n) {
		wmb();
		REG32(HA_RX(0) + 0xc) = hrx_cons;
		REG32(HA_TX(4) + 8) = hret_prod;
	}
	return n;
}

static void ring_desc(struct libranpu_wlan_ring *r, u8 kind, u8 band,
		      u8 link, u16 entries, u16 size, u32 regs, u32 base)
{
	r->kind = kind;
	r->band = band;
	r->link = link;
	r->entries = entries;
	r->entry_size = size;
	r->regs = regs;
	r->base = base;
}

static s32 wlan_ctl(u16 op, struct libranpu_wlan_audit *a)
{
	struct libranpu_wlan_ctl w = {
		.radio = 0, .dir = LIBRANPU_WLAN_RX | LIBRANPU_WLAN_TX,
	};

	return cmd(LIBRANPU_SVC_WLAN, op, &w, sizeof(w), a, NULL);
}

/* the host gives back n ids it held across the attach */
static void host_ret_held(u32 n)
{
	volatile u32 *ret = (u32 *)HRET_BASE;
	u32 id;

	for (id = 0; id < POOL_IDS && n; id++) {
		if (!held(id))
			continue;
		held_map[id / 32] &= ~BIT(id % 32);
		ret[hret_prod] = id;
		hret_prod = (hret_prod + 1) % HRET_ENTRIES;
		n--;
	}
	wmb();
	REG32(HA_TX(4) + 8) = hret_prod;
}

/* nheld ids stay with the host from before; half come back mid-way */
static void wlan_session(u32 frames, bool force, u32 nheld)
{
	struct libranpu_wlan_attach a;
	struct libranpu_wlan_attach_rsp rsp;
	struct libranpu_wlan_audit au;
	struct libranpu_wlan_stats ws;
	struct libranpu_wlan_tx_stats wt;
	struct libranpu_wlan_ctl w = { 0 };
	u32 i, t0, seen = 0, want = 0, band = 0, bound0 = fe.bound, bound;
	s32 st;

	memset(&a, 0, sizeof(a));
	memset(&chip, 0, sizeof(chip));
	memset((void *)HRX_BASE, 0, HRX_ENTRIES * 24);
	hrx_cons = hret_prod = host_got = host_bad = chip_held = 0;
	memset((void *)HELD_BASE, 0, POOL_IDS / 8);
	for (i = 0; i < nheld; i++)
		held_map[(3 + 7 * i) / 32] |= BIT((3 + 7 * i) % 32);
	for (i = 0; i < 4; i++)
		REG32(HA_RX(0) + 4 * i) = 0, REG32(HA_TX(4) + 4 * i) = 0,
		REG32(HA_TX(5) + 4 * i) = 0, REG32(HA_TX(6) + 4 * i) = 0,
		REG32(HA_RX(2) + 4 * i) = 0;
	for (i = 0; i < 28; i++)
		REG32(CHIP_REGS + 4 * i) = 0;
	htx_prod[0] = htx_prod[1] = htx_seq[0] = htx_seq[1] = 0;
	chip.tx_didx[0] = chip.tx_didx[1] = TX_DIDX0;
	REG32(regs_of(3) + 0xC) = TX_DIDX0;
	REG32(regs_of(4) + 0xC) = TX_DIDX0;

	a.backend = LIBRANPU_WLAN_RRO31;
	a.bands = 2;
	a.nrings = 12;
	a.link_win[0] = 1;
	a.link_win[1] = 0;
	a.pool_base = POOL_BASE;
	a.pool_ids = POOL_IDS;
	a.rx_headroom = HEADROOM;
	a.rx_buf_len = BUF_LEN;
	a.rx_held = nheld ? HELD_BASE : 0;
	a.rx_mod_frames = 16;
	a.tx_pool_base = LAN_POOL;
	a.npu_tokens = LAN_TOKENS;
	a.flags = force ? LIBRANPU_WLAN_F_FORCE_HOST : 0;
	ring_desc(&a.ring[0], LIBRANPU_RING_RX_DATA, 0, 0, RX_ENTRIES, 16,
		  regs_of(0), 0);
	ring_desc(&a.ring[1], LIBRANPU_RING_RX_DATA, 1, 1, RX_ENTRIES, 16,
		  regs_of(1), 0);
	ring_desc(&a.ring[2], LIBRANPU_RING_RXDMAD_C, 0, 0, RXD_ENTRIES, 16,
		  regs_of(2), 0);
	ring_desc(&a.ring[3], LIBRANPU_RING_HOST_RX, 0, 0, HRX_ENTRIES, 24, 0,
		  HRX_BASE);
	ring_desc(&a.ring[4], LIBRANPU_RING_HOST_RET, 0, 0, HRET_ENTRIES, 4, 4,
		  HRET_BASE);
	/* band 1 tx on link 0, as on the two-link chip */
	ring_desc(&a.ring[5], LIBRANPU_RING_TX_DATA, 0, 0, TX_ENTRIES, 16,
		  regs_of(3), 0);
	ring_desc(&a.ring[6], LIBRANPU_RING_TX_DATA, 1, 0, TX_ENTRIES, 16,
		  regs_of(4), 0);
	ring_desc(&a.ring[7], LIBRANPU_RING_HOST_TX, 0, 0, HTX_ENTRIES, 16, 5,
		  HTX_BASE);
	ring_desc(&a.ring[8], LIBRANPU_RING_HOST_TX, 1, 0, HTX_ENTRIES, 16, 6,
		  HTX_BASE + 0x4000);
	ring_desc(&a.ring[9], LIBRANPU_RING_TXFREE, 0, 0, TXF_ENTRIES, 16,
		  regs_of(5), TXFD_BASE);
	ring_desc(&a.ring[10], LIBRANPU_RING_TXFREE, 1, 0, TXF_ENTRIES, 16,
		  regs_of(6), TXFD_BASE + 16 * TXF_ENTRIES);
	a.ring[9].buf64 = a.ring[10].buf64 = TXF_BUF64;
	/*
	 * The driver armed its tx free rings but one slot, at its cpu
	 * index; the chip is part way in.
	 */
	for (i = 0; i < 2 * TXF_ENTRIES; i++) {
		volatile u32 *d = (u32 *)(TXFD_BASE + 16 * i);
		bool hole = i % TXF_ENTRIES == TXF_DIDX0 - 1;

		d[0] = hole ? 0 : TXFB_BASE + 2048 * i;
		d[1] = hole ? BIT(31) : TXF_BUF64 * 64 << 16;
		d[2] = d[3] = 0;
	}
	for (i = 0; i < 2; i++) {
		REG32(regs_of(5 + i) + 8) = TXF_DIDX0 - 1;
		REG32(regs_of(5 + i) + 0xC) = TXF_DIDX0;
	}
	ring_desc(&a.ring[11], LIBRANPU_RING_HOST_TXFREE, 0, 0, HTXF_ENTRIES,
		  8, 2, HTXF_BASE);

	st = cmd(LIBRANPU_SVC_WLAN, LIBRANPU_WLAN_ATTACH, &a, sizeof(a),
		 &rsp, NULL);
	CHECK(st == 0, "attach %d", (int)st);
	if (st)
		return;
	CHECK(rsp.ring_base[0] && rsp.ring_base[1] && rsp.ring_base[2] &&
	      !rsp.ring_base[3], "bases %x %x %x", rsp.ring_base[0],
	      rsp.ring_base[1], rsp.ring_base[2]);
	/* QEMU: bus address is the RAM address below 512 MB */
	chip.rx_desc[0] = rsp.ring_base[0] | 0x80000000;
	chip.rx_desc[1] = rsp.ring_base[1] | 0x80000000;
	chip.rxd_desc = rsp.ring_base[2] | 0x80000000;
	chip.tx_desc[0] = rsp.ring_base[5] | 0x80000000;
	chip.tx_desc[1] = rsp.ring_base[6] | 0x80000000;
	chip.txf_desc[0] = TXFD_BASE;
	chip.txf_desc[1] = TXFD_BASE + 16 * TXF_ENTRIES;
	chip.txf_idx[0] = chip.txf_idx[1] = TXF_DIDX0;
	CHECK(!rsp.ring_base[9] && !rsp.ring_base[10], "tx free placed");
	chip.npend[0] = chip.npend[1] = 0;
	chip.reports[0] = chip.reports[1] = 0;
	htxf_cons = htxf_tok[0] = htxf_tok[1] = 0;
	htxf_stat[0] = htxf_stat[1] = htxf_bad = htxf_other = 0;
	chip.other_sent[0] = chip.other_sent[1] = chip.others = 0;
	chip.lan_taken = chip.lan_bad = chip.txf_hold = 0;
	/* the frame engine carries on where the last session left it */
	lan_didx[0] = REG32(TDMA_RX(0) + 0xC);
	lan_didx[1] = REG32(TDMA_RX(1) + 0xC);
	lan_sent = 0;
	chip.tx_taken = chip.tx_bad = 0;
	/* link 0 on window 1: its span covers rx band 0 and RXDMAD_C */
	CHECK(REG32(NPU_MMIO_BASE + 0x13008) <= rsp.ring_base[0] &&
	      REG32(NPU_MMIO_BASE + 0x1300c) >= rsp.ring_base[2] + 16 * RXD_ENTRIES,
	      "window 1 %x-%x", REG32(NPU_MMIO_BASE + 0x13008),
	      REG32(NPU_MMIO_BASE + 0x1300c));

	CHECK(wlan_ctl(LIBRANPU_WLAN_START, NULL) == 0, "start");
	CHECK(REG32(regs_of(0) + 8) == RX_ENTRIES - 1, "rx0 cidx %u",
	      REG32(regs_of(0) + 8));
	CHECK(REG32(regs_of(3) + 8) == TX_DIDX0, "tx0 cidx %u",
	      REG32(regs_of(3) + 8));

	/* single frames on both bands, then 3-segment chains */
	for (i = 0, t0 = cycles(); i < frames; ) {
		bool chain = i >= frames / 2;
		u32 segs = chain ? 3 : 1, s;
		/* one repeat and one big-gap release in ten single frames */
		u32 ind = chain ? 0 : i % 10 == 5 ? 1 : i % 10 == 7 ? 6 : 0;

		if (i == frames / 3)
			host_ret_held(nheld / 2);
		/* host tx on both bands; the chip takes slots now and then */
		host_tx(i & 1, 1 + i % 5);
		if (i % 3 == 0)
			chip_tx_step();
		/* the driver is late for a while: the NPU must wait, not drop */
		htxf_hold = i > frames / 2 && i < frames / 2 + 200;
		host_txfree_poll();
		/* LAN to WiFi; the chip holds its reports: tokens run out */
		fe_lan(i & 1, 1 + i % 3);
		chip.txf_hold = i > frames / 5 && i < frames / 5 + 1500;
		if (chip_rxd_used() + segs < RXD_ENTRIES - 8) {
			for (s = 0; s < segs; s++)
				while (chip_rx(band, 60 + (i % 1400), s == segs - 1,
					       i, ind) != 0)
					host_rx_poll(&seen);
			want += ind != 1;
			band ^= 1;
			i++;
		}
		host_rx_poll(&seen);
		if (cycles() - t0 > TIMEOUT) {
			CHECK(0, "rx stuck at %u", i);
			return;
		}
	}
	for (t0 = cycles(); seen + fe.bound - bound0 < want &&
	     cycles() - t0 < TIMEOUT;)
		host_rx_poll(&seen);
	bound = fe.bound - bound0;
	CHECK(seen + bound == want && !host_bad && !fe.bad,
	      "frames %u + bound %u of %u, bad %u, fe bad %u", seen, bound, want,
	      host_bad, fe.bad);
	CHECK(force ? !bound : bound > 0, "force %d bound %u", force, bound);
	CHECK(host_got == chip.sent - chip.stale - bound,
	      "segments %u of %u - %u - %u", host_got, chip.sent, chip.stale,
	      bound);
	CHECK(cmd(LIBRANPU_SVC_WLAN, LIBRANPU_WLAN_GET_STATS, &w, sizeof(w),
		  &ws, NULL) == 0 && ws.rx_frames == chip.sent &&
	      ws.rx_stale == chip.stale && ws.rx_ind[6] == chip.gap &&
	      ws.host_segs == host_got && !ws.rx_pn_fail &&
	      ws.ppe_bound == bound && !ws.ppe_bad_id,
	      "stats frames %u stale %u gap %u segs %u bound %u", ws.rx_frames,
	      ws.rx_stale, ws.rx_ind[6], ws.host_segs, ws.ppe_bound);

	/* the chip takes what is left of the host's tx and reports it */
	chip.txf_hold = 0;
	for (t0 = cycles(); cycles() - t0 < TIMEOUT &&
	     (REG32(HA_TX(5) + 0xC) != htx_prod[0] ||
	      REG32(HA_TX(6) + 0xC) != htx_prod[1] ||
	      chip.tx_taken != htx_seq[0] + htx_seq[1] ||
	      htxf_tok[0] + htxf_tok[1] != chip.tx_taken ||
	      htxf_stat[0] + htxf_stat[1] != chip.reports[0] + chip.reports[1] ||
	      chip.lan_taken != lan_sent);) {
		chip_tx_step();
		chip_txfree(0, true);
		chip_txfree(1, true);
		host_txfree_poll();
	}
	CHECK(htxf_tok[0] == htx_seq[0] && htxf_tok[1] == htx_seq[1] &&
	      htxf_stat[0] == chip.reports[0] &&
	      htxf_stat[1] == chip.reports[1] && !htxf_bad &&
	      htxf_other == chip.others && chip.others,
	      "tx free tokens %u %u of %u %u, status %u %u of %u %u, bad %u",
	      htxf_tok[0], htxf_tok[1], htx_seq[0], htx_seq[1], htxf_stat[0],
	      htxf_stat[1], chip.reports[0], chip.reports[1], htxf_bad);
	CHECK(chip.tx_taken == htx_seq[0] + htx_seq[1] && !chip.tx_bad,
	      "tx taken %u of %u, bad %u", chip.tx_taken,
	      htx_seq[0] + htx_seq[1], chip.tx_bad);
	CHECK(chip.lan_taken == lan_sent && !chip.lan_bad && lan_sent > frames,
	      "lan frames %u of %u, bad %u", chip.lan_taken, lan_sent,
	      chip.lan_bad);
	w.page = 1;
	CHECK(cmd(LIBRANPU_SVC_WLAN, LIBRANPU_WLAN_GET_STATS, &w, sizeof(w),
		  &wt, NULL) == 0 && wt.descs[0] == htx_seq[0] &&
	      wt.descs[1] == htx_seq[1] && !wt.txfree_bad &&
	      wt.txfree_host == htx_seq[0] + htx_seq[1] &&
	      wt.txfree_events[0] + wt.txfree_events[1] ==
	      chip.reports[0] + chip.reports[1] + chip.others &&
	      wt.txfree_other == chip.others &&
	      wt.lan_frames[0] + wt.lan_frames[1] == lan_sent &&
	      wt.txfree_npu == lan_sent && wt.lan_no_token && !wt.lan_bad,
	      "tx stats %u %u full %u %u rewrite %u, tx free %u %u host %u bad %u full %u, lan %u %u npu %u no token %u ring full %u bad %u",
	      wt.descs[0], wt.descs[1], wt.full[0], wt.full[1], wt.rewrite,
	      wt.txfree_events[0], wt.txfree_events[1], wt.txfree_host,
	      wt.txfree_bad, wt.txfree_full, wt.lan_frames[0], wt.lan_frames[1],
	      wt.txfree_npu, wt.lan_no_token, wt.lan_ring_full, wt.lan_bad);
	out("tx: host %u, lan %u (no token %u, ring full %u), tx free host ring full %u\n",
	    wt.descs[0] + wt.descs[1], lan_sent, wt.lan_no_token,
	    wt.lan_ring_full, wt.txfree_full);
	w.page = 0;

	/* let the buffer task take the last returns, then stop */
	for (t0 = cycles(); cycles() - t0 < 20000000;)
		;
	st = wlan_ctl(LIBRANPU_WLAN_STOP, &au);
	CHECK(st == 0, "stop %d", (int)st);
	CHECK(au.free + au.chip + au.host == POOL_IDS &&
	      au.host == nheld - nheld / 2 && !au.lost && !au.transit &&
	      !au.fe && !chip_held && !au.tx_chip && !au.tx_host &&
	      !au.lan_tokens,
	      "audit free %u chip %u host %u transit %u fe %u lost %u, chip had %u held",
	      au.free, au.chip, au.host, au.transit, au.fe, au.lost, chip_held);
	out("wlan: %u frames to host, %u bound (%u segments), audit free %u chip %u host %u, tx %u\n",
	    seen, bound, host_got, au.free, au.chip, au.host, chip.tx_taken);
	CHECK(wlan_ctl(LIBRANPU_WLAN_DETACH, &au) == 0, "detach");
}

static void test_wlan(void)
{
	struct libranpu_wlan_attach a;

	memset(&a, 0, sizeof(a));
	a.backend = LIBRANPU_WLAN_RRO31;
	a.bands = 2;
	a.pool_base = POOL_BASE;
	a.pool_ids = POOL_IDS;
	CHECK(cmd(LIBRANPU_SVC_WLAN, LIBRANPU_WLAN_ATTACH, &a, sizeof(a),
		  NULL, NULL) == -EINVAL, "attach without rings");
	a.rx_headroom = 1024;
	a.rx_buf_len = 1536;
	CHECK(cmd(LIBRANPU_SVC_WLAN, LIBRANPU_WLAN_ATTACH, &a, sizeof(a),
		  NULL, NULL) == -EINVAL, "attach past the buffer");
	CHECK(wlan_ctl(LIBRANPU_WLAN_START, NULL) == -EBUSY, "start detached");

	FE(0xFE0) = 0;
	FE(0x80C) = 0;
	wlan_session(3000, false, 0);
	/* a second attach starts from scratch; all to the host */
	wlan_session(1000, true, 0);
	/* the host still holds ids from before: the chip never gets them */
	wlan_session(2000, false, 100);
}

void host_main(void)
{
	out("host: an7583 image at %x\n", IMG_ADDR);
	boot();
	test_ctl();
	test_burst();
	test_dbg();
	test_wlan();
	test_reset();
	finish();
}

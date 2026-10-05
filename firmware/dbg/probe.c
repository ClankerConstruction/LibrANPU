// SPDX-License-Identifier: GPL-3.0-only
/*
 * Debug service: memory peek/poke and hardware probes. Hart 0 hands a
 * probe to its hart over an SPSC ring and answers when it returns.
 */

#include "fw/csr.h"
#include "fw/errno.h"
#include "fw/lib.h"
#include "core/arena.h"
#include "core/spsc.h"
#include "core/trap.h"
#include "ctl/cmd.h"
#include "dbg/dbg.h"
#include "dbg/probe.h"
#include "plat/plat.h"

#define PROBE_TIMEOUT_US	200000
#define PEEK_MAX_WORDS		(LIBRANPU_CMD_PAYLOAD / 4)
#define LOAD_MAX		4096
#define WFI_DELAY_DEFAULT	100000

enum { STAGE_IDLE, STAGE_ARMED, STAGE_HALF, STAGE_DONE };

struct probe_req {
	struct cmd_ref ref;
	u32 probe;
	u32 arg[4];
	u32 res[8];
	s32 status;
	u32 stage;			/* target writes */
	u32 ack;			/* hart 0 writes */
	u32 t0;				/* hart 0 cycles at start */
	u32 t_armed;
	bool busy;
};

struct probe_chan {
	struct spsc_prod to;		/* hart 0 side */
	struct spsc_cons from;
	struct spsc_cons in;		/* target side */
	struct spsc_prod out;
};

static struct probe_req reqs[SOC_HARTS];
static struct probe_chan chans[SOC_HARTS];

extern char __scratch[];

int probe_init(void)
{
	u32 h, sz = spsc_bytes(4, 4);

	for (h = 0; h < SOC_HARTS; h++) {
		struct spsc *a = arena_alloc(&npu_sram, sz, 32, OWNER_DBG);
		struct spsc *b = arena_alloc(&npu_sram, sz, 32, OWNER_DBG);

		if (!a || !b)
			return -ENOMEM;
		spsc_init(a, 4, 4);
		spsc_init(b, 4, 4);
		spsc_prod_init(&chans[h].to, a);
		spsc_cons_init(&chans[h].in, a);
		spsc_prod_init(&chans[h].out, b);
		spsc_cons_init(&chans[h].from, b);
	}
	return 0;
}

/* ---- target side ------------------------------------------------ */

static void probe_cycles(struct probe_req *r)
{
	u32 n = r->arg[0] ? r->arg[0] : 1000, t0, i;
	volatile u32 x = 0;

	t0 = cycles();
	for (i = 0; i < n; i++)
		x += i;
	r->res[0] = cycles() - t0;
	r->res[1] = plat_cpu_mhz();
	r->res[2] = n;
}

/* two passes: the first fills the cache, the last shows hits */
static void probe_load(struct probe_req *r)
{
	u32 addr = r->arg[0], stride = r->arg[1], n = r->arg[2], pass, i;
	volatile u32 sink = 0;

	if (!n || n > LOAD_MAX || (addr | stride) & 3) {
		r->status = -EINVAL;
		return;
	}
	trap_catch(true);
	for (pass = 0; pass < 2; pass++) {
		u32 t0 = cycles();

		for (i = 0; i < n; i++)
			sink += *(volatile u32 *)(uintptr_t)(addr + i * stride);
		r->res[pass] = cycles() - t0;
	}
	trap_catch(false);
	r->res[2] = trap_caught();
	(void)sink;
}

/* does wfi stop the hart, and does its wake source bring it back */
static void probe_wfi(struct probe_req *r, u32 hart)
{
	u32 t0;

	plat_wake_arm(hart);
	r->res[1] = csr_read(mip);
	t0 = cycles();
	WRITE_ONCE(r->stage, STAGE_ARMED);
	wfi();
	r->res[0] = cycles() - t0;
	r->res[2] = csr_read(mip);
	r->res[3] = plat_wake_ack(hart);
	plat_wake_disarm(hart);
	r->res[4] = READ_ONCE(r->ack);	/* 0: woke before the raise */
}

/*
 * this hart reads a line hart 0 wrote uncached, writes it cached,
 * then writes it back once hart 0 has looked.
 */
static void probe_xhart(struct probe_req *r)
{
	volatile u32 *line = plat_cached(__scratch);
	u32 t0;

	r->res[0] = *line;
	*line = r->arg[1];
	mb();
	WRITE_ONCE(r->stage, STAGE_HALF);

	for (t0 = cycles(); !READ_ONCE(r->ack);)
		if (cycles() - t0 > PROBE_TIMEOUT_US * plat_cpu_mhz()) {
			r->status = -ETIMEDOUT;
			break;
		}
	plat_dcache_wb_inv((const void *)line);
	mb();
}

/* the first offset that aliases our own word, or faults */
static void probe_cluster(struct probe_req *r)
{
	volatile u32 *own = &dbgblk->rsv[0];
	u32 rel = (uintptr_t)own - NPU_CLUSTER_BASE, s, v, c;

	r->res[1] = 0;
	trap_catch(true);
	for (s = 0x4000; s <= 0x40000; s <<= 1) {
		volatile u32 *far = (u32 *)(uintptr_t)(NPU_CLUSTER_BASE + s + rel);

		v = *far;
		c = trap_caught();
		if (c) {
			r->res[1] = 2;
			r->res[2] = c;
			break;
		}
		*own = v ^ 0x5a5a5a5a;
		mb();
		if (*far == (v ^ 0x5a5a5a5a)) {
			*own = v ^ 0xa5a5a5a5;
			mb();
			if (*far == (v ^ 0xa5a5a5a5)) {
				r->res[1] = 1;
				break;
			}
		}
	}
	trap_catch(false);
	*own = 0;
	r->res[0] = s;
}

/* fault injection: no trap_catch, so health and the host see it */
static void probe_trap(void)
{
	__asm__ volatile("unimp");
}

/* the runner loop stops, so does the hart's heartbeat */
static void probe_hang(struct probe_req *r)
{
	u32 t0 = cycles(), limit = r->arg[0] * plat_cpu_mhz();

	while (!r->arg[0] || cycles() - t0 < limit)
		barrier();
}

static void probe_run(struct probe_req *r, u32 hart)
{
	switch (r->probe) {
	case LIBRANPU_PROBE_CYCLES:
		probe_cycles(r);
		break;
	case LIBRANPU_PROBE_LOAD:
		probe_load(r);
		break;
	case LIBRANPU_PROBE_WFI:
		probe_wfi(r, hart);
		break;
	case LIBRANPU_PROBE_XHART:
		probe_xhart(r);
		break;
	case LIBRANPU_PROBE_CLUSTER:
		probe_cluster(r);
		break;
	case LIBRANPU_PROBE_TRAP:
		probe_trap();
		break;
	case LIBRANPU_PROBE_HANG:
		probe_hang(r);
		break;
	default:
		r->status = -EOPNOTSUPP;
	}
}

/* ---- hart 0 side ------------------------------------------------ */

static void probe_answer(u32 hart)
{
	struct probe_req *r = &reqs[hart];
	struct libranpu_dbg_probe_rsp rsp;

	memcpy(rsp.res, r->res, sizeof(rsp.res));
	r->busy = false;
	cmd_finish(&r->ref, r->status, &rsp, sizeof(rsp));
}

/* the steps a probe needs from hart 0 while it runs */
static void probe_assist(u32 hart)
{
	struct probe_req *r = &reqs[hart];
	u32 stage = READ_ONCE(r->stage), mhz = plat_cpu_mhz();
	volatile u32 *line;

	switch (r->probe) {
	case LIBRANPU_PROBE_WFI:
		if (stage != STAGE_ARMED || READ_ONCE(r->ack))
			break;
		if (!r->t_armed)
			r->t_armed = cycles() | 1;
		if (cycles() - r->t_armed >= r->arg[0]) {
			r->res[5] = cycles() - r->t_armed;
			WRITE_ONCE(r->ack, 1);
			plat_wake_raise(hart);
		}
		break;
	case LIBRANPU_PROBE_XHART:
		if (stage != STAGE_HALF || READ_ONCE(r->ack))
			break;
		line = plat_cached(__scratch);
		r->res[2] = *line;			/* our cache: missed */
		r->res[3] = *(volatile u32 *)plat_uncached(__scratch);
		mb();
		WRITE_ONCE(r->ack, 1);
		break;
	}

	if (cycles() - r->t0 > PROBE_TIMEOUT_US * mhz) {
		/* the hart stays stuck; its request slot stays busy */
		r->status = -ETIMEDOUT;
		cmd_finish(&r->ref, -ETIMEDOUT, NULL, 0);
		r->ref.slot = 0xffff;
	}
}

static void probe_collect(u32 hart)
{
	struct probe_req *r = &reqs[hart];
	u32 idx;

	if (!spsc_pop32(&chans[hart].from, &idx))
		return;

	if (r->probe == LIBRANPU_PROBE_XHART) {
		volatile u32 *line = plat_cached(__scratch);

		r->res[4] = *(volatile u32 *)plat_uncached(__scratch);
		plat_dcache_inv((const void *)line);
		r->res[5] = *line;
		r->res[6] = r->arg[0];
		r->res[7] = r->arg[1];
	}
	if (r->ref.slot == 0xffff)
		r->busy = false;	/* answered by the timeout */
	else
		probe_answer(hart);
}

static int dbg_probe(struct cmd_ctx *c)
{
	const struct libranpu_dbg_probe *p = (const void *)c->req;
	struct probe_req *r;
	volatile u32 *line;

	if (p->hart >= SOC_HARTS || p->probe >= LIBRANPU_PROBE_MAX)
		return -EINVAL;
	if ((p->probe == LIBRANPU_PROBE_WFI ||
	     p->probe == LIBRANPU_PROBE_XHART) && !p->hart)
		return -EINVAL;
	if (dbg_hart_state_get(p->hart) != LIBRANPU_HART_RUN)
		return -EIO;

	r = &reqs[p->hart];
	if (r->busy)
		return -EBUSY;

	memset(r, 0, sizeof(*r));
	r->ref = c->ref;
	r->probe = p->probe;
	memcpy(r->arg, p->arg, sizeof(r->arg));
	r->busy = true;
	r->t0 = cycles();

	if (r->probe == LIBRANPU_PROBE_WFI && !r->arg[0])
		r->arg[0] = WFI_DELAY_DEFAULT;
	if (r->probe == LIBRANPU_PROBE_XHART) {
		/* DRAM holds A, our cache holds nothing */
		line = plat_cached(__scratch);
		r->arg[0] = 0xa0000000 | (c->ref.seq & 0xffff);
		r->arg[1] = 0xb0000000 | (c->ref.seq & 0xffff);
		*(volatile u32 *)plat_uncached(__scratch) = r->arg[0];
		mb();
		plat_dcache_inv((const void *)line);
	}

	wmb();
	if (!spsc_push32(&chans[p->hart].to, p->hart)) {
		r->busy = false;
		return -EBUSY;
	}
	return CMD_ASYNC;
}

static int dbg_peek(struct cmd_ctx *c)
{
	const struct libranpu_dbg_peek *p = (const void *)c->req;
	u32 i, cause;

	if (p->words > PEEK_MAX_WORDS || p->addr & 3)
		return -EINVAL;

	trap_catch(true);
	for (i = 0; i < p->words; i++)
		c->rsp[i] = *(volatile u32 *)(uintptr_t)(p->addr + 4 * i);
	trap_catch(false);
	cause = trap_caught();
	if (cause)
		return -EFAULT;
	c->rsp_len = p->words * 4;
	return 0;
}

static int dbg_poke(struct cmd_ctx *c)
{
	const struct libranpu_dbg_poke *p = (const void *)c->req;

	if (p->addr & 3)
		return -EINVAL;
	trap_catch(true);
	*(volatile u32 *)(uintptr_t)p->addr = p->val;
	trap_catch(false);
	return trap_caught() ? -EFAULT : 0;
}

static const struct cmd_handler dbg_handlers[] = {
	{ LIBRANPU_DBG_PEEK, sizeof(struct libranpu_dbg_peek), dbg_peek },
	{ LIBRANPU_DBG_POKE, sizeof(struct libranpu_dbg_poke), dbg_poke },
	{ LIBRANPU_DBG_PROBE, sizeof(struct libranpu_dbg_probe), dbg_probe },
};

const struct cmd_service dbg_service = {
	dbg_handlers, ARRAY_SIZE(dbg_handlers)
};

int probe_task(struct task *t, int budget)
{
	u32 hart = (uintptr_t)t->ctx, h, idx;
	int n = 0;

	(void)budget;
	if (!hart) {
		for (h = 0; h < SOC_HARTS; h++) {
			if (!reqs[h].busy)
				continue;
			probe_collect(h);
			if (reqs[h].busy && reqs[h].ref.slot != 0xffff)
				probe_assist(h);
			n++;
		}
	}

	if (spsc_pop32(&chans[hart].in, &idx)) {
		probe_run(&reqs[hart], hart);
		mb();
		WRITE_ONCE(reqs[hart].stage, STAGE_DONE);
		spsc_push32(&chans[hart].out, hart);
		n++;
	}
	return n;
}

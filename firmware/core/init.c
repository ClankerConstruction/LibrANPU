// SPDX-License-Identifier: GPL-3.0-only
/*
 * Hart bring-up. Hart 0 resets the NPU bus, sets up the runtime and
 * the host channel, then releases the others and reports ready.
 */

#include "fw/csr.h"
#include "fw/lib.h"
#include "core/arena.h"
#include "core/coremap.h"
#include "core/task.h"
#include "ctl/boot.h"
#include "dbg/dbg.h"

extern char __bss_start[], __bss_end[], __hart_start[], __hart_end[];

/* in .data: the host loads it as 0, hart 0 never clears it */
static volatile u32 boot_gate __section(".data");

/* covers the bus reset; mcycle only, no bus access meanwhile */
#define RESET_WINDOW_CYCLES	6000000
#define HARTS_UP_CYCLES		20000000

/* a line the last image left in this hart's D-cache must not come back */
static void hart_local_drop(void)
{
	char *p;

	for (p = __hart_start; p < __hart_end; p += 64)
		plat_dcache_inv(plat_cached(p));
}

static u32 wait_harts(void)
{
	u32 t0 = cycles(), up;

	do {
		u32 h;

		up = BIT(0);
		for (h = 1; h < SOC_HARTS; h++)
			if (dbg_hart_state_get(h) == LIBRANPU_HART_RUN)
				up |= BIT(h);
	} while (up != BIT(SOC_HARTS) - 1 &&
		 cycles() - t0 < HARTS_UP_CYCLES);

	return up;
}

static void __noreturn hart0_start(void)
{
	u32 t0 = cycles(), bb, up;
	int err;

	bb = plat_boot_block_addr();
	plat_reset_bus();

	memset(__bss_start, 0, __bss_end - __bss_start);
	memset(plat_uncached(__hart_start), 0, __hart_end - __hart_start);
	dbg_init();
	dbg_hart_state(0, LIBRANPU_HART_INIT);
	arena_init(&npu_sram, NPU_SRAM_BASE, SOC_SRAM_SIZE);
	arena_init(&cluster_sram, (uintptr_t)__bss_end,
		   NPU_CLUSTER_BASE + SOC_DBG_OFFSET - (uintptr_t)__bss_end);
	plat_init();

	err = boot_open(bb);
	if (!err)
		err = coremap_build();

	wmb();
	boot_gate = 1;
	up = wait_harts();

	boot_ready(err, up, cycles() - t0);
	dbg_trace(LIBRANPU_TRACE_BOOT, err, up);
	if (err)
		runner_park(BIT(0));
	runner(0);
}

void __noreturn fw_start(u32 hart)
{
	u32 t0;

	hart_local_drop();
	if (!hart)
		hart0_start();

	for (t0 = cycles(); cycles() - t0 < RESET_WINDOW_CYCLES;)
		;
	while (!boot_gate)
		;
	rmb();
	dbg_hart_state(hart, LIBRANPU_HART_INIT);
	runner(hart);
}

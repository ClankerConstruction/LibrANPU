// SPDX-License-Identifier: GPL-3.0-only
/*
 * Boot handshake. The host polls its own memory for "ready", so it
 * never touches NPU registers while the bus is being reset.
 */

#include "fw/errno.h"
#include "fw/lib.h"
#include "ctl/boot.h"
#include "ctl/cmd.h"
#include "ctl/evt.h"
#include "fw/info.h"
#include "plat/plat.h"

volatile struct libranpu_boot *boot;

static bool ring_ok(u32 entries, u32 min, u32 max)
{
	return IS_POW2(entries) && entries >= min && entries <= max;
}

int boot_open(u32 pa)
{
	volatile struct libranpu_boot *b;
	u32 ncmd, nevt;
	void *cmd, *evt;

	b = plat_host_ptr(pa, sizeof(*b));
	if (!b || b->magic != LIBRANPU_BOOT_MAGIC)
		return -EFAULT;
	boot = b;

	con_enable(b->flags & LIBRANPU_BOOT_F_UART);
	con_printf("fw %u.%u.%u abi %u.%u\n", FW_VERSION_MAJOR,
		   FW_VERSION_MINOR, FW_VERSION_PATCH, LIBRANPU_ABI_MAJOR,
		   LIBRANPU_ABI_MINOR);

	if (b->abi_major != LIBRANPU_ABI_MAJOR)
		return -EINVAL;

	ncmd = b->cmd_entries;
	nevt = b->evt_entries;
	if (!ring_ok(ncmd, 2, 256) || !ring_ok(nevt, 2 * ncmd, 4096))
		return -EINVAL;

	cmd = plat_host_ptr(b->cmd_ring, ncmd * LIBRANPU_CMD_SIZE);
	evt = plat_host_ptr(b->evt_ring, nevt * LIBRANPU_EVT_SIZE);
	if (!cmd || !evt)
		return -EFAULT;

	b->cmd_cons = b->cmd_prod;
	b->evt_prod = 0;
	evt_init(evt, nevt, ncmd);
	cmd_init(cmd, ncmd);
	return 0;
}

void boot_ready(int status, u32 harts_up, u32 boot_cycles)
{
	volatile struct libranpu_boot *b = boot;

	if (!b)
		return;

	b->status = status;
	b->fw_version = FW_VERSION;
	b->harts_up = harts_up;
	b->boot_cycles = boot_cycles;
	b->cpu_mhz = plat_cpu_mhz();
	con_printf("ready %d, harts %x, %u cycles\n", status, harts_up,
		   boot_cycles);
	wmb();
	b->ready = LIBRANPU_BOOT_READY;
}

void boot_fault(u32 hart, u32 mcause)
{
	volatile struct libranpu_boot *b = boot;

	if (!b || hart >= ARRAY_SIZE(b->fault))
		return;
	b->fault[hart] = mcause | LIBRANPU_FAULT_VALID;
	wmb();
	plat_notify_host();
}

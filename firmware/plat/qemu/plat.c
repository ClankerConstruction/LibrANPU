// SPDX-License-Identifier: GPL-3.0-only
/* QEMU virt test platform: RAM windows, 16550 UART, CLINT wake */

#include "fw/csr.h"
#include "plat/plat.h"

#define CLINT_MSIP(h)		(QEMU_CLINT + 4 * (h))

u32 plat_boot_block_addr(void)
{
	return REG32(REG_MIB(MIB_BOOT_BLOCK));
}

void plat_reset_bus(void)
{
}

void plat_init(void)
{
}

void *plat_host_ptr(u64 pa, u32 len)
{
	if (pa < QEMU_RAM_BASE || pa + len > QEMU_RAM_BASE + QEMU_RAM_SIZE ||
	    pa + len < pa)
		return NULL;
	return (void *)(uintptr_t)pa;
}

void *plat_cached(void *p)
{
	return p;
}

void *plat_uncached(void *p)
{
	return p;
}

void plat_dcache_inv(const void *line)
{
	(void)line;
}

void plat_dcache_wb_inv(const void *line)
{
	(void)line;
}

u32 plat_cpu_mhz(void)
{
	return 1000;
}

void plat_putc(char c)
{
	while (!(*(volatile u8 *)(QEMU_UART + 5) & 0x20))
		;
	*(volatile u8 *)QEMU_UART = c;
}

u32 plat_hart_pc(u32 hart)
{
	(void)hart;
	return 0;
}

u32 plat_doorbell(void)
{
	return REG32(REG_MBQ_CTRL(0, 2));
}

void plat_notify_host(void)
{
	REG32(REG_MBQ_CTRL(MBQ_TO_HOST, 2)) += 1;
}

void plat_wake_arm(u32 hart)
{
	REG32(CLINT_MSIP(hart)) = 0;
	csr_set(mie, MIE_MSIE);
}

void plat_wake_disarm(u32 hart)
{
	(void)hart;
	csr_clear(mie, MIE_MSIE);
}

void plat_wake_raise(u32 hart)
{
	REG32(CLINT_MSIP(hart)) = 1;
}

u32 plat_wake_ack(u32 hart)
{
	REG32(CLINT_MSIP(hart)) = 0;
	return 1;
}

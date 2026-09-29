// SPDX-License-Identifier: GPL-3.0-only
/* LibrANPU platform */

#include "fw/csr.h"
#include "plat/plat.h"

/* NPU window onto host DRAM: 0x80000000-0xbfffffff */
#define HOST_WIN_BASE		0x80000000ull
#define HOST_WIN_SIZE		0x40000000ull
#define UNCACHED(a)		(((a) & 0x3fffffff) | 0x40000000)

static u32 doorbell_seen;

u32 plat_boot_block_addr(void)
{
	return REG32(REG_MIB(MIB_BOOT_BLOCK));
}

/*
 * Reset the NPU internal bus: mailbox, MIB and host adaptor state.
 * Nothing may touch NPU registers meanwhile, the host included.
 */
void plat_reset_bus(void)
{
	u32 t;

	REG32(REG_RSTCTRL1) = RSTCTRL1_NPU_BUS;
	for (t = cycles(); cycles() - t < 1000000;)
		;
	REG32(REG_RSTCTRL1) = 0;
	for (t = cycles(); cycles() - t < 1000000;)
		;
	REG32(REG_THREAD_ENABLE) = 4;
	REG32(REG_THREAD_ENABLE) = 1;
	for (t = cycles(); cycles() - t < 1000000;)
		;
}

void plat_init(void)
{
	u32 q;

	/* queue 8 interrupts the host, queue n hart n */
	REG32(REG_MBOX_INT_MASK(0)) = BIT(MBQ_TO_HOST);
	for (q = 0; q < SOC_HARTS; q++)
		REG32(REG_MBOX_INT_MASK(q + 1)) = BIT(q);
	doorbell_seen = REG32(REG_MBQ_CTRL(0, 2));
}

void *plat_host_ptr(u64 pa, u32 len)
{
	if (pa < HOST_WIN_BASE || pa + len > HOST_WIN_BASE + HOST_WIN_SIZE ||
	    pa + len < pa)
		return NULL;
	return (void *)(uintptr_t)UNCACHED((u32)pa);
}

void *plat_cached(void *p)
{
	return (void *)(uintptr_t)(((uintptr_t)p & 0x3fffffff) | 0x80000000);
}

void *plat_uncached(void *p)
{
	return (void *)(uintptr_t)UNCACHED((uintptr_t)p);
}

/* custom line ops on one 64-byte D-cache line: 0xFC2 and 0xFC0 */
void plat_dcache_inv(const void *line)
{
	__asm__ volatile(".insn i 0x73, 0, x0, %0, -62" :: "r"(line) : "memory");
}

void plat_dcache_wb_inv(const void *line)
{
	__asm__ volatile(".insn i 0x73, 0, x0, %0, -64" :: "r"(line) : "memory");
}

u32 plat_cpu_mhz(void)
{
	static const u16 freq[] = SOC_PLL_FREQS;
	static u32 mhz;
	u32 cfg;

	if (!mhz) {
		cfg = REG32(SOC_PLL_CFG);
		mhz = freq[(cfg >> SOC_PLL_SEL_SHIFT) & 3] / ((cfg & 7) + 1);
	}
	return mhz;
}

void plat_putc(char c)
{
	u32 n;

	/* give up rather than hang on a stuck UART */
	for (n = 0; n < 200000; n++)
		if (REG32(SOC_UART_LSR) & SOC_UART_THRE)
			break;
	REG32(SOC_UART_BASE) = (u8)c;
}

u32 plat_hart_pc(u32 hart)
{
	return REG32(REG_HART_PC(hart));
}

u32 plat_doorbell(void)
{
	return REG32(REG_MBQ_CTRL(0, 2));
}

void plat_notify_host(void)
{
	REG32(REG_MBQ_CTRL(MBQ_TO_HOST, 2)) += 1;
}

static void plic_set(u32 src, bool on)
{
	u32 w = (src + 1) >> 5, bit = BIT((src + 1) & 31);

	if (on) {
		REG32(PLIC_PRIO(src)) = 16;
		REG32(PLIC_MASK(w)) &= ~bit;
		REG32(PLIC_ENABLE(w)) |= bit;
	} else {
		REG32(PLIC_MASK(w)) |= bit;
		REG32(PLIC_ENABLE(w)) &= ~bit;
	}
}

/* the hart's own mailbox queue wakes it; mstatus.MIE stays off */
void plat_wake_arm(u32 hart)
{
	REG32(REG_MBOX_INT_STS) = BIT(hart);
	REG32(PLIC_THRESHOLD) = 0;
	plic_set(PLIC_SRC_MBOX(hart), true);
	csr_set(mie, MIE_MEIE);
}

void plat_wake_disarm(u32 hart)
{
	csr_clear(mie, MIE_MEIE);
	plic_set(PLIC_SRC_MBOX(hart), false);
}

void plat_wake_raise(u32 hart)
{
	REG32(REG_MBQ_CTRL(hart, 2)) += 1;
}

/* returns the claimed source, numbered from 1, or 0 */
u32 plat_wake_ack(u32 hart)
{
	u32 src = REG32(PLIC_CLAIM);

	REG32(REG_MBOX_INT_STS) = BIT(hart);
	if (src)
		REG32(PLIC_CLAIM) = src;
	return src;
}

void plat_pcie_window(u32 win, u32 base, u32 end)
{
	u32 reg = win ? SOC_PCIE_WIN1 : SOC_PCIE_WIN0;

	REG32(reg) = base;
	REG32(reg + 4) = end;
}

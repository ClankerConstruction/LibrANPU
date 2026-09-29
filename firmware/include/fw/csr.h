/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef __FW_CSR_H
#define __FW_CSR_H

#include "fw/types.h"

#define csr_read(csr) ({						\
	u32 __v;							\
	__asm__ volatile("csrr %0, " #csr : "=r"(__v) :: "memory");	\
	__v; })

#define csr_write(csr, v)						\
	__asm__ volatile("csrw " #csr ", %0" :: "rK"(v) : "memory")

#define csr_set(csr, v)							\
	__asm__ volatile("csrs " #csr ", %0" :: "rK"(v) : "memory")

#define csr_clear(csr, v)						\
	__asm__ volatile("csrc " #csr ", %0" :: "rK"(v) : "memory")

#define MSTATUS_MIE		BIT(3)
#define MIE_MSIE		BIT(3)
#define MIE_MTIE		BIT(7)
#define MIE_MEIE		BIT(11)
#define MCAUSE_IRQ		BIT(31)

static inline u32 hart_id(void)
{
	return csr_read(mhartid);
}

static inline u32 cycles(void)
{
	return csr_read(mcycle);
}

/* both halves, re-read when the low word wrapped between */
static inline u64 cycles64(void)
{
	u32 hi, lo;

	do {
		hi = csr_read(mcycleh);
		lo = csr_read(mcycle);
	} while (hi != csr_read(mcycleh));

	return (u64)hi << 32 | lo;
}

static inline void wfi(void)
{
	__asm__ volatile("wfi" ::: "memory");
}

#endif

/* SPDX-License-Identifier: GPL-3.0-only */
/* The cluster's boot logic: wait for the trigger, jump to BOOT_BASE */

#include "map.h"

#define BOOT_TRIGGER	(NPU_MMIO_BASE + 0x6000)
#define BOOT_CONFIG	(NPU_MMIO_BASE + 0x6004)
#define BOOT_BASE	(NPU_MMIO_BASE + 0x6020)

	.section .text
	.globl _start
_start:
	/* 0xf0000000-0xffffffff faults, M-mode too: a locked PMP entry */
	li	t0, 0x3dffffff
	csrw	pmpaddr0, t0
	li	t0, 0x98
	csrw	pmpcfg0, t0
	csrr	a0, mhartid
	li	t0, BOOT_TRIGGER
	li	t1, BOOT_CONFIG
1:	lw	t2, 0(t0)
	beqz	t2, 1b
	lw	t2, 0(t1)
	srl	t2, t2, a0
	andi	t2, t2, 1
	beqz	t2, 1b
	li	t0, BOOT_BASE
	slli	t1, a0, 2
	add	t0, t0, t1
	lw	t0, 0(t0)
	fence.i
	jr	t0

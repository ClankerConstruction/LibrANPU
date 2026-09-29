/* SPDX-License-Identifier: GPL-3.0-only */
/* Every hart starts here: gp, trap vector, its stack, then fw_start */

#include "soc.h"

#define STACK_SHIFT	12

	.section .text.entry, "ax", @progbits
	.globl _start
_start:
	.option push
	.option norelax
	la	gp, __global_pointer$
	.option pop
	csrw	mie, zero
	csrci	mstatus, 8
	la	t0, trap_entry
	csrw	mtvec, t0

	csrr	a0, mhartid
	li	t1, SOC_HARTS
	bgeu	a0, t1, 1f

	la	sp, __stacks
	addi	t0, a0, 1
	slli	t0, t0, STACK_SHIFT
	add	sp, sp, t0
	call	fw_start

	/* a hart the image does not use */
1:	wfi
	j	1b

/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Code, rodata, stacks, hart-private state and scratch in DRAM; data
 * and bss in cluster SRAM below the debug block.
 */
#include "soc.h"
#include "map.h"

#define STACK_SIZE	0x1000
#define SCRATCH_SIZE	0x10000

OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY
{
	DRAM (rwx) : ORIGIN = FW_CODE_BASE, LENGTH = FW_CODE_SIZE
	CLUSTER (rw) : ORIGIN = NPU_CLUSTER_BASE, LENGTH = SOC_DBG_OFFSET
}

SECTIONS
{
	.text : {
		KEEP(*(.text.entry))
		*(.text.hot .text.hot.*)
		*(.text .text.*)
	} > DRAM

	.rodata : ALIGN(8) {
		*(.rodata .rodata.* .srodata .srodata.*)
	} > DRAM

	. = ALIGN(8);
	__code_end = .;

	/* before .bss: its input sections match .bss.* too */
	.hart (NOLOAD) : ALIGN(64) {
		__hart_start = .;
		*(.bss.hart)
		. = ALIGN(64);
		__hart_end = .;
	} > DRAM

	.data : ALIGN(8) {
		__data_start = .;
		*(.sdata .sdata.*)
		*(.data .data.*)
		. = ALIGN(8);
		__data_end = .;
	} > CLUSTER

	__global_pointer$ = __data_start + 0x800;

	.bss (NOLOAD) : ALIGN(8) {
		__bss_start = .;
		*(.sbss .sbss.*)
		*(.bss .bss.*)
		*(COMMON)
		. = ALIGN(8);
		__bss_end = .;
	} > CLUSTER

	.stacks (NOLOAD) : ALIGN(64) {
		__stacks = .;
		. += SOC_HARTS * STACK_SIZE;
	} > DRAM

	.scratch (NOLOAD) : ALIGN(64) {
		__scratch = .;
		. += SCRATCH_SIZE;
		__scratch_end = .;
	} > DRAM

	__dram_end = .;

	/DISCARD/ : {
		*(.eh_frame .eh_frame_hdr .comment .note .note.* .riscv.attributes)
	}
}

ASSERT(__bss_end <= NPU_CLUSTER_BASE + SOC_DBG_OFFSET, "globals overlap the debug block")

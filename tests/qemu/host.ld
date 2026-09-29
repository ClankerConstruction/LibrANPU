OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS
{
	. = 0x88000000;
	.text : { *(.text.entry) *(.text .text.*) }
	.rodata : { *(.rodata .rodata.* .srodata .srodata.*) }
	.data : { *(.data .data.* .sdata .sdata.*) }
	.bss : { *(.bss .bss.* .sbss .sbss.* COMMON) }
	. = ALIGN(16);
	. += 0x4000;
	__stack_top = .;
	/DISCARD/ : { *(.eh_frame .comment .riscv.attributes) }
}

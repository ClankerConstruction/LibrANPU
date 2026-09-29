	.section .text.entry
	.globl _start
_start:
	la	sp, __stack_top
	call	host_main
1:	j	1b

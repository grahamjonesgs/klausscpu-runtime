	.section .head.text, "ax"
	.globl _start
_start:	setr r1, __bss_start
	setr r2, ext_sym
	call kmain
	halt
	.pushsection __ex_table, "a"
	.quad _start, kmain
	.popsection
	.pushsection .discard.foo, "a"
	.quad 0
	.popsection

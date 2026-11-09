/* .pushsection/.popsection with flags and @progbits/@nobits, special names */
	.text
	nop
	.pushsection __ex_table, "a"
	.balign 8
	.quad 1f, 2f
	.popsection
1:	nop
2:	nop
	.pushsection .discard.addressable, "aw", @progbits
	.quad 1b
	.popsection
	.pushsection ".data..percpu", "aw", @progbits
	.quad 0
	.popsection
	.section .bss..page_aligned, "aw", @nobits
	.space 64
	.section .init.text, "ax", @progbits
	nop
	.section .note.GNU-stack, "", @progbits
	.previous
	.subsection 1
	nop

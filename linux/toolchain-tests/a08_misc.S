/* misc kernel .S needs: .cfi_*, .loc-free asm, __ASSEMBLY__ cpp, .incbin, .section .init.ramfs, .local/.comm, .reloc */
#define __ASSEMBLY__ 1
	.text
	.globl fn
fn:
	.cfi_startproc
	push r15
	.cfi_adjust_cfa_offset 8
	pop r15
	ret
	.cfi_endproc
	.section .init.ramfs, "a"
	.incbin "a07_word.S"
	.local lsym
	.comm csym, 64, 8
	.lcomm lcs, 16

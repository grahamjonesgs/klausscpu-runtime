/* numeric local labels, .type/.size, .globl/.weak/.hidden, ENTRY-style */
	.text
	.globl fa
	.type fa, @function
	.p2align 3
fa:
1:	nop
	jmp 1b
	jmp 1f
1:	ret
	.size fa, . - fa
	.weak fb
	.hidden fc
fb:
fc:	ret
	.type obj, @object
	.data
obj:	.quad fa, fb + 8, 0
	.size obj, 24

/* relocations: absolute symbol refs in code (setr with a symbol), 32/64-bit data relocs, PC-relative calls, symbol differences */
	.text
	.globl ra
ra:	setr r1, ext_sym
	setr r2, ext_sym + 16
	call ext_fn
	callrel ext_fn
	jmp ext_fn
	ret
	.data
	.long ext_sym
	.quad ext_sym
	.long 2f - 1f
1:	.quad 0
2:
	.set ABS_ADDR, 0xF0010000
	.quad ABS_ADDR

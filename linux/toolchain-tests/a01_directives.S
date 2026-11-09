/* data and alignment directives used by kernel .S files and linker-visible tables */
	.section .rodata.a01, "a"
	.byte 1, 2
	.short 3
	.long 6
	.int 7
	.quad 8
	.8byte 9
	.4byte 10
	.2byte 11
	.balign 16
	.p2align 4
	.align 8
	.fill 3, 1, 0xff
	.zero 5
	.skip 4
	.org . + 8
	.ascii "abc"
	.asciz "def"
	.string "ghi"

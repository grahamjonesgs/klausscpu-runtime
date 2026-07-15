	.file	"repro.c"
	.section	.text.sha_pad,"ax",@progbits
	.globl	sha_pad                         # -- Begin function sha_pad
	.p2align	2
	.type	sha_pad,@function
sha_pad:                                # @sha_pad
# %bb.0:                                # %entry
	push	r15
	getsp	r15
	copy	r12, r1
	sextw	r12
	cmprv	r12, 1
	jmpltrel	.LBB0_2
# %bb.1:                                # %for.body.preheader
	leapc	r14, pad
	copy	r13, r1
	zextw	r13
.LBB0_4:                                # %for.body
                                        # =>This Inner Loop Header: Depth=1
	memget8	r11, r0
	memset8	r11, r14
	incr	r14
	incr	r0
	decr	r13
	jmpzrel	.LBB0_2
	jmprel	.LBB0_4
.LBB0_2:                                # %for.cond.cleanup
	leapc	r14, pad
	addr	r14, r14, r12
	setr	r13, 128
	memset8	r13, r14
	addi	r14, r1, 72
	copy	r13, r14
	sextw	r13
	shrv	r13, 57
	andv	r13, 63
	addr	r14, r14, r13
	setr	r13, -64
	andr	r14, r14, r13
	addi	r13, r14, -8
	copy	r11, r1
	incr	r11
	sextw	r13
	sextw	r11
	cmprr	r11, r13
	jmpgerel	.LBB0_5
# %bb.3:                                # %for.body11.preheader
	leapc	r10, pad
	addr	r11, r10, r11
	subr	r10, r1, r14
	addi	r10, r10, 9
	setr	r9, 0
.LBB0_6:                                # %for.body11
                                        # =>This Inner Loop Header: Depth=1
	memset8	r9, r11
	incr	r11
	zextw	r10
	incr	r10
	copy	r8, r10
	zextw	r8
	cmprr	r8, r10
	jmpnerel	.LBB0_5
	jmprel	.LBB0_6
.LBB0_5:                                # %for.cond.cleanup10
	leapc	r11, pad
	addr	r13, r11, r13
	copy	r10, r12
	shrv	r10, 53
	memset8	r10, r13
	sextw	r14
	addr	r14, r11, r14
	shlv	r12, 3
	stidx8	r12, r14, -1
	setsp	r15
	pop	r15
	ret
.Lfunc_end0:
	.size	sha_pad, .Lfunc_end0-sha_pad
                                        # -- End function
	.section	.text.main,"ax",@progbits
	.globl	main                            # -- Begin function main
	.p2align	2
	.type	main,@function
main:                                   # @main
# %bb.0:                                # %entry
	push	r15
	getsp	r15
	addsp	-24
	leapc	r0, main.m
	setr	r1, 0
	callrel	sha_pad
	leapc	r12, pad
	memget8	r12, r12
	setsp	r15
	pop	r15
	ret
.Lfunc_end1:
	.size	main, .Lfunc_end1-main
                                        # -- End function
	.type	pad,@object                     # @pad
	.section	.bss.pad,"aw",@nobits
pad:
	.zero	256
	.size	pad, 256

	.type	main.m,@object                  # @main.m
	.section	.bss.main.m,"aw",@nobits
main.m:
	.zero	8
	.size	main.m, 8

	.ident	"clang version 23.0.0git (https://github.com/grahamjonesgs/klausscpu-llvm.git 2fa6958fe09425038e3a874dcc79450a100d0a3b)"
	.section	".note.GNU-stack","",@progbits
	.addrsig
	.addrsig_sym main.m

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
	copy	r14, r1
	andv	r14, 3
	jmpltrel	.LBB0_7
# %bb.1:                                # %for.body.preheader
	copy	r11, r1
	zextw	r11
	cmprv	r11, 4
	jmpultrel	.LBB0_2
	jmprel	.LBB0_3
.LBB0_2:
	setr	r13, 0
	jmprel	.LBB0_5
.LBB0_3:                                # %for.body.preheader.new
	setr	r13, 0
	copy	r10, r0
	incr	r10
	andv	r11, 2147483644
.LBB0_14:                               # %for.body
                                        # =>This Inner Loop Header: Depth=1
	leapc	r9, pad
	addr	r9, r9, r13
	addr	r8, r10, r13
	ldidx8	r2, r8, -1
	memset8	r2, r9
	memget8	r2, r8
	stidx8	r2, r9, 1
	ldidx8	r2, r8, 1
	stidx8	r2, r9, 2
	ldidx8	r8, r8, 2
	stidx8	r8, r9, 3
	addi	r13, r13, 4
	cmprr	r11, r13
	jmperel	.LBB0_4
	jmprel	.LBB0_14
.LBB0_4:                                # %for.cond.cleanup.loopexit.unr-lcssa
	cmprv	r14, 0
	jmperel	.LBB0_7
.LBB0_5:                                # %for.body.epil.preheader
	addr	r11, r0, r13
	leapc	r10, pad
	addr	r13, r10, r13
	copy	r10, r14
.LBB0_6:                                # %for.body.epil
                                        # =>This Inner Loop Header: Depth=1
	memget8	r9, r11
	memset8	r9, r13
	incr	r11
	incr	r13
	decr	r10
	jmpnzrel	.LBB0_6
.LBB0_7:                                # %for.cond.cleanup
	leapc	r13, pad
	addr	r13, r13, r12
	setr	r11, 128
	memset8	r11, r13
	addi	r13, r1, 72
	copy	r11, r13
	sextw	r11
	shrv	r11, 57
	andv	r11, 63
	addr	r13, r13, r11
	setr	r11, -64
	andr	r13, r13, r11
	addi	r11, r13, -8
	copy	r10, r1
	incr	r10
	sextw	r11
	sextw	r10
	cmprr	r10, r11
	jmpgerel	.LBB0_15
# %bb.8:                                # %for.body11.preheader
	subr	r9, r13, r1
	addi	r9, r9, -10
	cmprv	r14, 3
	jmperel	.LBB0_12
# %bb.10:                               # %for.body11.prol.preheader
	setr	r8, 1
	setr	r0, 0
.LBB0_11:                               # %for.body11.prol
                                        # =>This Inner Loop Header: Depth=1
	leapc	r1, pad
	addr	r1, r1, r10
	memset8	r0, r1
	xorr	r1, r14, r8
	incr	r8
	incr	r10
	zextw	r1
	cmprv	r1, 3
	jmpnerel	.LBB0_11
.LBB0_12:                               # %for.body11.prol.loopexit
	zextw	r9
	cmprv	r9, 3
	jmpultrel	.LBB0_15
# %bb.13:                               # %for.body11.preheader49
	leapc	r14, pad
	addr	r9, r10, r14
	subr	r14, r10, r13
	addi	r14, r14, 8
	addi	r10, r9, 3
	setr	r9, 0
.LBB0_16:                               # %for.body11
                                        # =>This Inner Loop Header: Depth=1
	memset8	r9, r10
	stidx8	r9, r10, -1
	stidx8	r9, r10, -2
	stidx8	r9, r10, -3
	addi	r10, r10, 4
	addi	r14, r14, 4
	copy	r8, r14
	zextw	r8
	cmprv	r8, 0
	jmperel	.LBB0_15
	jmprel	.LBB0_16
.LBB0_15:                               # %for.cond.cleanup10
	leapc	r14, pad
	addr	r11, r14, r11
	copy	r10, r12
	shrv	r10, 53
	memset8	r10, r11
	sextw	r13
	addr	r14, r14, r13
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

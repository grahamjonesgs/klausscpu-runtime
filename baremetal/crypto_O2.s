	.file	"crypto.c"
	.section	.text.main,"ax",@progbits
	.globl	main                            # -- Begin function main
	.p2align	2
	.type	main,@function
main:                                   # @main
# %bb.0:                                # %check_hex.exit252
	push	r15
	getsp	r15
	addsp	-272
	stidx64	r4, r15, -8                     # 8-byte Folded Spill
	stidx64	r5, r15, -16                    # 8-byte Folded Spill
	stidx64	r6, r15, -24                    # 8-byte Folded Spill
	stidx64	r7, r15, -32                    # 8-byte Folded Spill
	leapc	r12, g_fail
	setr	r5, 0
	memset32	r5, r12
	leapc	r7, g_pass
	memset32	r5, r7
	leapc	r0, .L.str
	callrel	printf
	leapc	r4, .L.str.37
	leapc	r1, .L.str.1
	copy	r0, r4
	callrel	printf
	leapc	r6, .L.str.38
	copy	r0, r6
	callrel	printf
	memget32	r12, r7
	incr	r12
	memset32	r12, r7
	leapc	r1, .L.str.2
	copy	r0, r4
	callrel	printf
	copy	r0, r6
	callrel	printf
	memget32	r12, r7
	incr	r12
	memset32	r12, r7
	leapc	r1, .L.str.3
	copy	r0, r4
	callrel	printf
	copy	r0, r6
	callrel	printf
	memget32	r12, r7
	incr	r12
	memset32	r12, r7
	leapc	r1, .L.str.4
	copy	r0, r4
	callrel	printf
	copy	r0, r6
	callrel	printf
	memget32	r12, r7
	incr	r12
	memset32	r12, r7
	leapc	r1, .L.str.5
	copy	r0, r4
	callrel	printf
	copy	r0, r6
	callrel	printf
	memget32	r12, r7
	incr	r12
	memset32	r12, r7
	leapc	r0, .L.str.6
	addi	r4, r15, -96
	copy	r1, r5
	copy	r2, r4
	callrel	sha256
	leapc	r12, .L.str.8
	addi	r12, r12, 3
.LBB0_8:                                # %for.body.i253
                                        # =>This Inner Loop Header: Depth=1
	addr	r14, r4, r5
	memget8	r13, r14
	copy	r11, r13
	shrv	r11, 4
	leapc	r10, hexcmp.hexd
	addr	r11, r10, r11
	leapc	r6, g_fail
	leapc	r2, .L.str.42
	memget8	r11, r11
	ldidx8	r10, r12, -3
	cmprr	r10, r11
	jmpnerel	.LBB0_10
# %bb.9:                                # %if.end.i
                                        #   in Loop: Header=BB0_8 Depth=1
	andv	r13, 15
	leapc	r11, hexcmp.hexd
	addr	r13, r11, r13
	leapc	r6, g_fail
	leapc	r2, .L.str.42
	memget8	r13, r13
	ldidx8	r11, r12, -2
	cmprr	r11, r13
	jmperel	.LBB0_1
	jmprel	.LBB0_10
.LBB0_1:                                # %for.cond.i
                                        #   in Loop: Header=BB0_8 Depth=1
	ldidx8	r14, r14, 1
	copy	r13, r14
	shrv	r13, 4
	leapc	r11, hexcmp.hexd
	addr	r13, r11, r13
	leapc	r6, g_fail
	leapc	r2, .L.str.42
	memget8	r13, r13
	ldidx8	r11, r12, -1
	cmprr	r11, r13
	jmpnerel	.LBB0_10
# %bb.2:                                # %if.end.i.1
                                        #   in Loop: Header=BB0_8 Depth=1
	andv	r14, 15
	leapc	r13, hexcmp.hexd
	addr	r14, r13, r14
	leapc	r6, g_fail
	leapc	r2, .L.str.42
	memget8	r14, r14
	memget8	r13, r12
	cmprr	r13, r14
	jmpnerel	.LBB0_10
# %bb.3:                                # %for.cond.i.1
                                        #   in Loop: Header=BB0_8 Depth=1
	addi	r14, r15, -96
	addr	r14, r14, r5
	ldidx8	r13, r14, 2
	copy	r11, r13
	shrv	r11, 4
	leapc	r10, hexcmp.hexd
	addr	r11, r10, r11
	leapc	r6, g_fail
	leapc	r2, .L.str.42
	memget8	r11, r11
	ldidx8	r10, r12, 1
	cmprr	r10, r11
	jmpnerel	.LBB0_10
# %bb.4:                                # %if.end.i.2
                                        #   in Loop: Header=BB0_8 Depth=1
	andv	r13, 15
	leapc	r11, hexcmp.hexd
	addr	r13, r11, r13
	leapc	r6, g_fail
	leapc	r2, .L.str.42
	memget8	r13, r13
	ldidx8	r11, r12, 2
	cmprr	r11, r13
	jmpnerel	.LBB0_10
# %bb.5:                                # %for.cond.i.2
                                        #   in Loop: Header=BB0_8 Depth=1
	ldidx8	r14, r14, 3
	copy	r13, r14
	shrv	r13, 4
	leapc	r11, hexcmp.hexd
	addr	r13, r11, r13
	leapc	r6, g_fail
	leapc	r2, .L.str.42
	memget8	r13, r13
	ldidx8	r11, r12, 3
	cmprr	r11, r13
	jmpnerel	.LBB0_10
# %bb.6:                                # %if.end.i.3
                                        #   in Loop: Header=BB0_8 Depth=1
	andv	r14, 15
	leapc	r13, hexcmp.hexd
	addr	r14, r13, r14
	leapc	r6, g_fail
	leapc	r2, .L.str.42
	memget8	r14, r14
	ldidx8	r13, r12, 4
	cmprr	r13, r14
	jmpnerel	.LBB0_10
# %bb.7:                                # %for.cond.i.3
                                        #   in Loop: Header=BB0_8 Depth=1
	leapc	r6, g_pass
	leapc	r2, .L.str.41
	addi	r12, r12, 8
	addi	r5, r5, 4
	cmprv	r5, 32
	jmperel	.LBB0_10
	jmprel	.LBB0_8
.LBB0_10:                               # %hexcmp.exit
	leapc	r0, .L.str.40
	leapc	r1, .L.str.7
	callrel	printf
	memget32	r12, r6
	incr	r12
	memset32	r12, r6
	leapc	r0, .L.str.9
	setr	r1, 3
	copy	r2, r4
	callrel	sha256
	setr	r12, 0
	leapc	r14, .L.str.11
	addi	r14, r14, 3
.LBB0_18:                               # %for.body.i260
                                        # =>This Inner Loop Header: Depth=1
	addr	r13, r4, r12
	memget8	r11, r13
	copy	r10, r11
	shrv	r10, 4
	leapc	r9, hexcmp.hexd
	addr	r10, r9, r10
	leapc	r5, g_fail
	leapc	r2, .L.str.42
	memget8	r10, r10
	ldidx8	r9, r14, -3
	cmprr	r9, r10
	jmpnerel	.LBB0_20
# %bb.19:                               # %if.end.i269
                                        #   in Loop: Header=BB0_18 Depth=1
	andv	r11, 15
	leapc	r10, hexcmp.hexd
	addr	r11, r10, r11
	leapc	r5, g_fail
	leapc	r2, .L.str.42
	memget8	r11, r11
	ldidx8	r10, r14, -2
	cmprr	r10, r11
	jmperel	.LBB0_11
	jmprel	.LBB0_20
.LBB0_11:                               # %for.cond.i275
                                        #   in Loop: Header=BB0_18 Depth=1
	ldidx8	r13, r13, 1
	copy	r11, r13
	shrv	r11, 4
	leapc	r10, hexcmp.hexd
	addr	r11, r10, r11
	leapc	r5, g_fail
	leapc	r2, .L.str.42
	memget8	r11, r11
	ldidx8	r10, r14, -1
	cmprr	r10, r11
	jmpnerel	.LBB0_20
# %bb.12:                               # %if.end.i269.1
                                        #   in Loop: Header=BB0_18 Depth=1
	andv	r13, 15
	leapc	r11, hexcmp.hexd
	addr	r13, r11, r13
	leapc	r5, g_fail
	leapc	r2, .L.str.42
	memget8	r13, r13
	memget8	r11, r14
	cmprr	r11, r13
	jmpnerel	.LBB0_20
# %bb.13:                               # %for.cond.i275.1
                                        #   in Loop: Header=BB0_18 Depth=1
	addi	r13, r15, -96
	addr	r13, r13, r12
	ldidx8	r11, r13, 2
	copy	r10, r11
	shrv	r10, 4
	leapc	r9, hexcmp.hexd
	addr	r10, r9, r10
	leapc	r5, g_fail
	leapc	r2, .L.str.42
	memget8	r10, r10
	ldidx8	r9, r14, 1
	cmprr	r9, r10
	jmpnerel	.LBB0_20
# %bb.14:                               # %if.end.i269.2
                                        #   in Loop: Header=BB0_18 Depth=1
	andv	r11, 15
	leapc	r10, hexcmp.hexd
	addr	r11, r10, r11
	leapc	r5, g_fail
	leapc	r2, .L.str.42
	memget8	r11, r11
	ldidx8	r10, r14, 2
	cmprr	r10, r11
	jmpnerel	.LBB0_20
# %bb.15:                               # %for.cond.i275.2
                                        #   in Loop: Header=BB0_18 Depth=1
	ldidx8	r13, r13, 3
	copy	r11, r13
	shrv	r11, 4
	leapc	r10, hexcmp.hexd
	addr	r11, r10, r11
	leapc	r5, g_fail
	leapc	r2, .L.str.42
	memget8	r11, r11
	ldidx8	r10, r14, 3
	cmprr	r10, r11
	jmpnerel	.LBB0_20
# %bb.16:                               # %if.end.i269.3
                                        #   in Loop: Header=BB0_18 Depth=1
	andv	r13, 15
	leapc	r11, hexcmp.hexd
	addr	r13, r11, r13
	leapc	r5, g_fail
	leapc	r2, .L.str.42
	memget8	r13, r13
	ldidx8	r11, r14, 4
	cmprr	r11, r13
	jmpnerel	.LBB0_20
# %bb.17:                               # %for.cond.i275.3
                                        #   in Loop: Header=BB0_18 Depth=1
	leapc	r5, g_pass
	leapc	r2, .L.str.41
	addi	r14, r14, 8
	addi	r12, r12, 4
	cmprv	r12, 32
	jmperel	.LBB0_20
	jmprel	.LBB0_18
.LBB0_20:                               # %hexcmp.exit278
	leapc	r0, .L.str.40
	leapc	r1, .L.str.10
	callrel	printf
	memget32	r12, r5
	incr	r12
	memset32	r12, r5
	leapc	r0, .L.str.12
	setr	r1, 1
	copy	r2, r4
	callrel	sha256
	setr	r12, 0
	leapc	r14, .L.str.14
	addi	r14, r14, 3
.LBB0_28:                               # %for.body.i284
                                        # =>This Inner Loop Header: Depth=1
	addr	r13, r4, r12
	memget8	r11, r13
	copy	r10, r11
	shrv	r10, 4
	leapc	r9, hexcmp.hexd
	addr	r10, r9, r10
	leapc	r5, g_fail
	leapc	r2, .L.str.42
	memget8	r10, r10
	ldidx8	r9, r14, -3
	cmprr	r9, r10
	jmpnerel	.LBB0_30
# %bb.29:                               # %if.end.i293
                                        #   in Loop: Header=BB0_28 Depth=1
	andv	r11, 15
	leapc	r10, hexcmp.hexd
	addr	r11, r10, r11
	leapc	r5, g_fail
	leapc	r2, .L.str.42
	memget8	r11, r11
	ldidx8	r10, r14, -2
	cmprr	r10, r11
	jmperel	.LBB0_21
	jmprel	.LBB0_30
.LBB0_21:                               # %for.cond.i299
                                        #   in Loop: Header=BB0_28 Depth=1
	ldidx8	r13, r13, 1
	copy	r11, r13
	shrv	r11, 4
	leapc	r10, hexcmp.hexd
	addr	r11, r10, r11
	leapc	r5, g_fail
	leapc	r2, .L.str.42
	memget8	r11, r11
	ldidx8	r10, r14, -1
	cmprr	r10, r11
	jmpnerel	.LBB0_30
# %bb.22:                               # %if.end.i293.1
                                        #   in Loop: Header=BB0_28 Depth=1
	andv	r13, 15
	leapc	r11, hexcmp.hexd
	addr	r13, r11, r13
	leapc	r5, g_fail
	leapc	r2, .L.str.42
	memget8	r13, r13
	memget8	r11, r14
	cmprr	r11, r13
	jmpnerel	.LBB0_30
# %bb.23:                               # %for.cond.i299.1
                                        #   in Loop: Header=BB0_28 Depth=1
	addi	r13, r15, -96
	addr	r13, r13, r12
	ldidx8	r11, r13, 2
	copy	r10, r11
	shrv	r10, 4
	leapc	r9, hexcmp.hexd
	addr	r10, r9, r10
	leapc	r5, g_fail
	leapc	r2, .L.str.42
	memget8	r10, r10
	ldidx8	r9, r14, 1
	cmprr	r9, r10
	jmpnerel	.LBB0_30
# %bb.24:                               # %if.end.i293.2
                                        #   in Loop: Header=BB0_28 Depth=1
	andv	r11, 15
	leapc	r10, hexcmp.hexd
	addr	r11, r10, r11
	leapc	r5, g_fail
	leapc	r2, .L.str.42
	memget8	r11, r11
	ldidx8	r10, r14, 2
	cmprr	r10, r11
	jmpnerel	.LBB0_30
# %bb.25:                               # %for.cond.i299.2
                                        #   in Loop: Header=BB0_28 Depth=1
	ldidx8	r13, r13, 3
	copy	r11, r13
	shrv	r11, 4
	leapc	r10, hexcmp.hexd
	addr	r11, r10, r11
	leapc	r5, g_fail
	leapc	r2, .L.str.42
	memget8	r11, r11
	ldidx8	r10, r14, 3
	cmprr	r10, r11
	jmpnerel	.LBB0_30
# %bb.26:                               # %if.end.i293.3
                                        #   in Loop: Header=BB0_28 Depth=1
	andv	r13, 15
	leapc	r11, hexcmp.hexd
	addr	r13, r11, r13
	leapc	r5, g_fail
	leapc	r2, .L.str.42
	memget8	r13, r13
	ldidx8	r11, r14, 4
	cmprr	r11, r13
	jmpnerel	.LBB0_30
# %bb.27:                               # %for.cond.i299.3
                                        #   in Loop: Header=BB0_28 Depth=1
	leapc	r5, g_pass
	leapc	r2, .L.str.41
	addi	r14, r14, 8
	addi	r12, r12, 4
	cmprv	r12, 32
	jmperel	.LBB0_30
	jmprel	.LBB0_28
.LBB0_30:                               # %hexcmp.exit302
	leapc	r0, .L.str.40
	leapc	r1, .L.str.13
	callrel	printf
	memget32	r12, r5
	incr	r12
	memset32	r12, r5
	leapc	r0, .L.str.15
	setr	r1, 56
	stidx64	r1, r15, -200                   # 8-byte Folded Spill
	copy	r2, r4
	callrel	sha256
	setr	r12, 0
	leapc	r14, .L.str.17
	addi	r14, r14, 3
	addi	r13, r4, 3
.LBB0_38:                               # %for.body.i308
                                        # =>This Inner Loop Header: Depth=1
	addr	r11, r4, r12
	memget8	r11, r11
	copy	r10, r11
	shrv	r10, 4
	leapc	r9, hexcmp.hexd
	addr	r10, r9, r10
	leapc	r5, g_fail
	leapc	r2, .L.str.42
	memget8	r10, r10
	ldidx8	r9, r14, -3
	cmprr	r9, r10
	jmpnerel	.LBB0_40
# %bb.39:                               # %if.end.i317
                                        #   in Loop: Header=BB0_38 Depth=1
	andv	r11, 15
	leapc	r10, hexcmp.hexd
	addr	r11, r10, r11
	leapc	r5, g_fail
	leapc	r2, .L.str.42
	memget8	r11, r11
	ldidx8	r10, r14, -2
	cmprr	r10, r11
	jmperel	.LBB0_31
	jmprel	.LBB0_40
.LBB0_31:                               # %for.cond.i323
                                        #   in Loop: Header=BB0_38 Depth=1
	addr	r11, r13, r12
	ldidx8	r10, r11, -2
	copy	r9, r10
	shrv	r9, 4
	leapc	r8, hexcmp.hexd
	addr	r9, r8, r9
	leapc	r5, g_fail
	leapc	r2, .L.str.42
	memget8	r9, r9
	ldidx8	r8, r14, -1
	cmprr	r8, r9
	jmpnerel	.LBB0_40
# %bb.32:                               # %if.end.i317.1
                                        #   in Loop: Header=BB0_38 Depth=1
	andv	r10, 15
	leapc	r9, hexcmp.hexd
	addr	r10, r9, r10
	leapc	r5, g_fail
	leapc	r2, .L.str.42
	memget8	r10, r10
	memget8	r9, r14
	cmprr	r9, r10
	jmpnerel	.LBB0_40
# %bb.33:                               # %for.cond.i323.1
                                        #   in Loop: Header=BB0_38 Depth=1
	ldidx8	r10, r11, -1
	copy	r9, r10
	shrv	r9, 4
	leapc	r8, hexcmp.hexd
	addr	r9, r8, r9
	leapc	r5, g_fail
	leapc	r2, .L.str.42
	memget8	r9, r9
	ldidx8	r8, r14, 1
	cmprr	r8, r9
	jmpnerel	.LBB0_40
# %bb.34:                               # %if.end.i317.2
                                        #   in Loop: Header=BB0_38 Depth=1
	andv	r10, 15
	leapc	r9, hexcmp.hexd
	addr	r10, r9, r10
	leapc	r5, g_fail
	leapc	r2, .L.str.42
	memget8	r10, r10
	ldidx8	r9, r14, 2
	cmprr	r9, r10
	jmpnerel	.LBB0_40
# %bb.35:                               # %for.cond.i323.2
                                        #   in Loop: Header=BB0_38 Depth=1
	memget8	r11, r11
	copy	r10, r11
	shrv	r10, 4
	leapc	r9, hexcmp.hexd
	addr	r10, r9, r10
	leapc	r5, g_fail
	leapc	r2, .L.str.42
	memget8	r10, r10
	ldidx8	r9, r14, 3
	cmprr	r9, r10
	jmpnerel	.LBB0_40
# %bb.36:                               # %if.end.i317.3
                                        #   in Loop: Header=BB0_38 Depth=1
	andv	r11, 15
	leapc	r10, hexcmp.hexd
	addr	r11, r10, r11
	leapc	r5, g_fail
	leapc	r2, .L.str.42
	memget8	r11, r11
	ldidx8	r10, r14, 4
	cmprr	r10, r11
	jmpnerel	.LBB0_40
# %bb.37:                               # %for.cond.i323.3
                                        #   in Loop: Header=BB0_38 Depth=1
	leapc	r5, g_pass
	leapc	r2, .L.str.41
	addi	r14, r14, 8
	addi	r12, r12, 4
	cmprv	r12, 32
	jmperel	.LBB0_40
	jmprel	.LBB0_38
.LBB0_40:                               # %base64_encode.exit534
	leapc	r0, .L.str.40
	leapc	r1, .L.str.16
	stidx64	r0, r15, -168                   # 8-byte Folded Spill
	callrel	printf
	memget32	r12, r5
	incr	r12
	memset32	r12, r5
	setr	r7, 0
	stidx8	r7, r15, -96
	leapc	r1, .L.str.6
	copy	r0, r4
	callrel	strcmp
	zextw	r12
	cmpeqr	r5, r12, r7
	leapc	r1, .L.str.18
	leapc	r2, .L.str.42
	leapc	r12, .L.str.41
	stidx64	r12, r15, -208                  # 8-byte Folded Spill
	cmprv	r5, 0
	stidx64	r2, r15, -176                   # 8-byte Folded Spill
	jmpe	.LBB0_42
# %bb.41:                               # %base64_encode.exit534
	ldidx64	r2, r15, -208                   # 8-byte Folded Reload
.LBB0_42:                               # %base64_encode.exit534
	ldidx64	r0, r15, -168                   # 8-byte Folded Reload
	callrel	printf
	leapc	r12, g_fail
	leapc	r14, g_pass
	stidx64	r14, r15, -216                  # 8-byte Folded Spill
	cmprv	r5, 0
	stidx64	r12, r15, -184                  # 8-byte Folded Spill
	jmpe	.LBB0_44
# %bb.43:                               # %base64_encode.exit534
	ldidx64	r12, r15, -216                  # 8-byte Folded Reload
.LBB0_44:                               # %base64_encode.exit534
	memget32	r14, r12
	incr	r14
	memset32	r14, r12
	stidx8	r7, r4, 4
	setr	r6, 61
	stidx8	r6, r4, 3
	stidx8	r6, r4, 2
	setr	r12, 103
	stidx64	r12, r15, -248                  # 8-byte Folded Spill
	stidx8	r12, r4, 1
	setr	r12, 90
	stidx64	r12, r15, -192                  # 8-byte Folded Spill
	stidx8	r12, r15, -96
	leapc	r1, .L.str.21
	copy	r0, r4
	callrel	strcmp
	zextw	r12
	cmpeqr	r5, r12, r7
	leapc	r1, .L.str.20
	cmprv	r5, 0
	ldidx64	r2, r15, -176                   # 8-byte Folded Reload
	jmpe	.LBB0_46
# %bb.45:                               # %base64_encode.exit534
	ldidx64	r2, r15, -208                   # 8-byte Folded Reload
.LBB0_46:                               # %base64_encode.exit534
	ldidx64	r0, r15, -168                   # 8-byte Folded Reload
	callrel	printf
	cmprv	r5, 0
	ldidx64	r12, r15, -184                  # 8-byte Folded Reload
	jmpe	.LBB0_48
# %bb.47:                               # %base64_encode.exit534
	ldidx64	r12, r15, -216                  # 8-byte Folded Reload
.LBB0_48:                               # %base64_encode.exit534
	memget32	r14, r12
	incr	r14
	memset32	r14, r12
	ldidx64	r12, r15, -200                  # 8-byte Folded Reload
	stidx8	r12, r4, 2
	stidx8	r7, r4, 4
	stidx8	r6, r4, 3
	setr	r12, 109
	stidx64	r12, r15, -224                  # 8-byte Folded Spill
	stidx8	r12, r4, 1
	ldidx64	r12, r15, -192                  # 8-byte Folded Reload
	stidx8	r12, r15, -96
	leapc	r1, .L.str.24
	copy	r0, r4
	callrel	strcmp
	zextw	r12
	cmpeqr	r5, r12, r7
	leapc	r1, .L.str.23
	cmprv	r5, 0
	ldidx64	r2, r15, -176                   # 8-byte Folded Reload
	jmpe	.LBB0_50
# %bb.49:                               # %base64_encode.exit534
	ldidx64	r2, r15, -208                   # 8-byte Folded Reload
.LBB0_50:                               # %base64_encode.exit534
	ldidx64	r0, r15, -168                   # 8-byte Folded Reload
	callrel	printf
	cmprv	r5, 0
	ldidx64	r12, r15, -184                  # 8-byte Folded Reload
	jmpe	.LBB0_52
# %bb.51:                               # %base64_encode.exit534
	ldidx64	r12, r15, -216                  # 8-byte Folded Reload
.LBB0_52:                               # %base64_encode.exit534
	memget32	r14, r12
	incr	r14
	memset32	r14, r12
	stidx8	r7, r4, 4
	setr	r12, 118
	stidx64	r12, r15, -200                  # 8-byte Folded Spill
	stidx8	r12, r4, 3
	setr	r12, 57
	stidx64	r12, r15, -232                  # 8-byte Folded Spill
	stidx8	r12, r4, 2
	ldidx64	r12, r15, -224                  # 8-byte Folded Reload
	stidx8	r12, r4, 1
	ldidx64	r12, r15, -192                  # 8-byte Folded Reload
	stidx8	r12, r15, -96
	leapc	r1, .L.str.27
	copy	r0, r4
	callrel	strcmp
	zextw	r12
	cmpeqr	r5, r12, r7
	leapc	r1, .L.str.26
	cmprv	r5, 0
	ldidx64	r2, r15, -176                   # 8-byte Folded Reload
	jmpe	.LBB0_54
# %bb.53:                               # %base64_encode.exit534
	ldidx64	r2, r15, -208                   # 8-byte Folded Reload
.LBB0_54:                               # %base64_encode.exit534
	ldidx64	r0, r15, -168                   # 8-byte Folded Reload
	callrel	printf
	cmprv	r5, 0
	ldidx64	r12, r15, -184                  # 8-byte Folded Reload
	jmpe	.LBB0_56
# %bb.55:                               # %base64_encode.exit534
	ldidx64	r12, r15, -216                  # 8-byte Folded Reload
.LBB0_56:                               # %base64_encode.exit534
	memget32	r14, r12
	incr	r14
	memset32	r14, r12
	stidx8	r6, r4, 7
	stidx64	r6, r15, -240                   # 8-byte Folded Spill
	stidx8	r6, r4, 6
	ldidx64	r12, r15, -248                  # 8-byte Folded Reload
	stidx8	r12, r4, 5
	stidx8	r7, r4, 8
	setr	r5, 89
	stidx8	r5, r4, 4
	ldidx64	r12, r15, -200                  # 8-byte Folded Reload
	stidx8	r12, r4, 3
	ldidx64	r12, r15, -232                  # 8-byte Folded Reload
	stidx8	r12, r4, 2
	ldidx64	r12, r15, -224                  # 8-byte Folded Reload
	stidx8	r12, r4, 1
	ldidx64	r12, r15, -192                  # 8-byte Folded Reload
	stidx8	r12, r15, -96
	leapc	r1, .L.str.30
	copy	r0, r4
	callrel	strcmp
	zextw	r12
	cmpeqr	r6, r12, r7
	leapc	r1, .L.str.29
	cmprv	r6, 0
	ldidx64	r2, r15, -176                   # 8-byte Folded Reload
	jmpe	.LBB0_58
# %bb.57:                               # %base64_encode.exit534
	ldidx64	r2, r15, -208                   # 8-byte Folded Reload
.LBB0_58:                               # %base64_encode.exit534
	ldidx64	r0, r15, -168                   # 8-byte Folded Reload
	callrel	printf
	cmprv	r6, 0
	ldidx64	r12, r15, -184                  # 8-byte Folded Reload
	jmpe	.LBB0_60
# %bb.59:                               # %base64_encode.exit534
	ldidx64	r12, r15, -216                  # 8-byte Folded Reload
.LBB0_60:                               # %base64_encode.exit534
	memget32	r14, r12
	incr	r14
	memset32	r14, r12
	setr	r12, 121
	stidx8	r12, r4, 7
	setr	r12, 70
	stidx8	r12, r4, 6
	stidx8	r5, r4, 4
	ldidx64	r12, r15, -200                  # 8-byte Folded Reload
	stidx8	r12, r4, 3
	ldidx64	r12, r15, -232                  # 8-byte Folded Reload
	stidx8	r12, r4, 2
	ldidx64	r12, r15, -224                  # 8-byte Folded Reload
	stidx8	r12, r4, 5
	stidx8	r12, r4, 1
	ldidx64	r12, r15, -192                  # 8-byte Folded Reload
	stidx8	r12, r15, -96
	stidx8	r7, r4, 8
	leapc	r6, .L.str.33
	copy	r0, r4
	copy	r1, r6
	callrel	strcmp
	zextw	r12
	cmpeqr	r5, r12, r7
	cmprv	r5, 0
	jmpe	.LBB0_62
# %bb.61:                               # %base64_encode.exit534
	ldidx64	r12, r15, -208                  # 8-byte Folded Reload
	stidx64	r12, r15, -176                  # 8-byte Folded Spill
.LBB0_62:                               # %base64_encode.exit534
	leapc	r1, .L.str.32
	ldidx64	r0, r15, -168                   # 8-byte Folded Reload
	ldidx64	r2, r15, -176                   # 8-byte Folded Reload
	callrel	printf
	cmprv	r5, 0
	jmpe	.LBB0_64
# %bb.63:                               # %base64_encode.exit534
	ldidx64	r12, r15, -216                  # 8-byte Folded Reload
	stidx64	r12, r15, -184                  # 8-byte Folded Spill
.LBB0_64:                               # %base64_encode.exit534
	ldidx64	r14, r15, -184                  # 8-byte Folded Reload
	memget32	r12, r14
	incr	r12
	memset32	r12, r14
	addi	r5, r15, -160
	copy	r14, r7
	copy	r12, r7
	jmprel	.LBB0_65
.LBB0_80:                               #   in Loop: Header=BB0_65 Depth=1
	addi	r14, r14, 6
	incr	r6
.LBB0_65:                               # %for.cond.i540
                                        # =>This Inner Loop Header: Depth=1
	memget8	r13, r6
	cmprv	r13, 0
	jmperel	.LBB0_66
# %bb.67:                               # %for.cond.i540
                                        #   in Loop: Header=BB0_65 Depth=1
	cmprv	r13, 61
	jmperel	.LBB0_68
# %bb.69:                               # %if.end.i543
                                        #   in Loop: Header=BB0_65 Depth=1
	addi	r11, r13, -65
	cmprv	r11, 26
	jmpultrel	.LBB0_79
# %bb.70:                               # %if.end.i.i
                                        #   in Loop: Header=BB0_65 Depth=1
	addi	r11, r13, -97
	andv	r11, 255
	cmprv	r11, 25
	jmpugtrel	.LBB0_72
# %bb.71:                               # %if.then5.i.i
                                        #   in Loop: Header=BB0_65 Depth=1
	addi	r11, r13, -71
	jmprel	.LBB0_79
.LBB0_72:                               # %if.end7.i.i
                                        #   in Loop: Header=BB0_65 Depth=1
	addi	r11, r13, -48
	andv	r11, 255
	cmprv	r11, 9
	jmpugtrel	.LBB0_74
# %bb.73:                               # %if.then11.i.i
                                        #   in Loop: Header=BB0_65 Depth=1
	addi	r11, r13, 4
	jmprel	.LBB0_79
.LBB0_74:                               # %if.end14.i.i
                                        #   in Loop: Header=BB0_65 Depth=1
	cmprv	r13, 43
	jmperel	.LBB0_75
# %bb.76:                               # %if.end14.i.i
                                        #   in Loop: Header=BB0_65 Depth=1
	cmprv	r13, 47
	jmpnerel	.LBB0_83
# %bb.78:                               # %if.end7.fold.split.i
                                        #   in Loop: Header=BB0_65 Depth=1
	setr	r11, 63
	jmprel	.LBB0_79
.LBB0_75:                               #   in Loop: Header=BB0_65 Depth=1
	setr	r11, 62
.LBB0_79:                               # %if.end7.i
                                        #   in Loop: Header=BB0_65 Depth=1
	shlv	r7, 6
	orr	r7, r11, r7
	copy	r13, r14
	sextw	r13
	cmprv	r13, 2
	jmpltrel	.LBB0_80
# %bb.81:                               # %if.then10.i
                                        #   in Loop: Header=BB0_65 Depth=1
	setr64	r13, -1, 0
	copy	r11, r12
	sextw	r11
	cmprv	r11, 63
	jmpgtrel	.LBB0_84
# %bb.82:                               # %if.end14.i
                                        #   in Loop: Header=BB0_65 Depth=1
	addr	r13, r5, r11
	copy	r11, r7
	zextw	r11
	addi	r14, r14, -2
	copy	r10, r14
	zextw	r10
	shrr	r11, r11, r10
	memset8	r11, r13
	incr	r12
.LBB0_83:                               # %for.inc.i
                                        #   in Loop: Header=BB0_65 Depth=1
	incr	r6
	jmprel	.LBB0_65
.LBB0_68:
	copy	r13, r12
	jmprel	.LBB0_84
.LBB0_66:
	copy	r13, r12
.LBB0_84:                               # %base64_decode.exit
	zextw	r13
	setr	r12, 6
	cmpner	r12, r13, r12
	ldidx8	r14, r15, -160
	andv	r14, 255
	setr	r13, 102
	stidx64	r13, r15, -168                  # 8-byte Folded Spill
	cmpner	r14, r14, r13
	orr	r12, r12, r14
	ldidx8	r14, r5, 1
	andv	r14, 255
	setr	r13, 111
	cmpner	r14, r14, r13
	orr	r12, r12, r14
	ldidx8	r14, r5, 2
	andv	r14, 255
	cmpner	r14, r14, r13
	orr	r12, r12, r14
	ldidx8	r14, r5, 3
	andv	r14, 255
	setr	r13, 98
	cmpner	r14, r14, r13
	orr	r12, r12, r14
	ldidx8	r14, r5, 4
	andv	r14, 255
	setr	r7, 97
	cmpner	r14, r14, r7
	orr	r12, r12, r14
	ldidx8	r14, r5, 5
	andv	r14, 255
	setr	r13, 114
	cmpner	r14, r14, r13
	orr	r6, r12, r14
	leapc	r2, .L.str.41
	leapc	r12, .L.str.42
	cmprv	r6, 0
	jmpe	.LBB0_86
# %bb.85:                               # %base64_decode.exit
	copy	r2, r12
.LBB0_86:                               # %base64_decode.exit
	leapc	r0, .L.str.40
	leapc	r1, .L.str.34
	callrel	printf
	leapc	r12, g_pass
	leapc	r14, g_fail
	cmprv	r6, 0
	jmpe	.LBB0_88
# %bb.87:                               # %base64_decode.exit
	copy	r12, r14
.LBB0_88:                               # %base64_decode.exit
	memget32	r14, r12
	incr	r14
	memset32	r14, r12
	ldidx64	r12, r15, -240                  # 8-byte Folded Reload
	stidx8	r12, r4, 23
	stidx8	r12, r4, 22
	stidx8	r7, r4, 19
	setr	r12, 105
	stidx8	r12, r4, 18
	setr	r12, 110
	stidx8	r12, r4, 17
	setr	r12, 86
	stidx8	r12, r4, 16
	setr	r12, 48
	stidx8	r12, r4, 15
	setr	r12, 73
	stidx8	r12, r4, 14
	setr	r12, 120
	stidx8	r12, r4, 13
	setr	r12, 55
	stidx8	r12, r4, 12
	setr	r12, 78
	stidx8	r12, r4, 11
	ldidx64	r12, r15, -200                  # 8-byte Folded Reload
	stidx8	r12, r4, 20
	stidx8	r12, r4, 10
	setr	r12, 54
	stidx8	r12, r4, 9
	setr	r12, 47
	stidx8	r12, r4, 8
	setr	r12, 43
	stidx8	r12, r4, 7
	setr	r12, 68
	stidx8	r12, r4, 6
	setr	r12, 52
	stidx8	r12, r4, 5
	ldidx64	r12, r15, -168                  # 8-byte Folded Reload
	stidx8	r12, r4, 4
	setr	r12, 67
	stidx8	r12, r4, 3
	setr	r12, 69
	stidx8	r12, r4, 2
	setr	r12, 65
	stidx8	r12, r4, 21
	stidx8	r12, r4, 1
	stidx8	r12, r15, -96
	setr	r14, 0
	stidx8	r14, r4, 24
	copy	r13, r14
	copy	r12, r14
	jmprel	.LBB0_89
.LBB0_102:                              #   in Loop: Header=BB0_89 Depth=1
	addi	r13, r13, 6
	incr	r4
.LBB0_89:                               # %for.cond.i605
                                        # =>This Inner Loop Header: Depth=1
	memget8	r11, r4
	cmprv	r11, 0
	jmperel	.LBB0_106
# %bb.90:                               # %for.cond.i605
                                        #   in Loop: Header=BB0_89 Depth=1
	cmprv	r11, 61
	jmperel	.LBB0_106
# %bb.91:                               # %if.end.i611
                                        #   in Loop: Header=BB0_89 Depth=1
	addi	r10, r11, -65
	cmprv	r10, 26
	jmpultrel	.LBB0_101
# %bb.92:                               # %if.end.i.i614
                                        #   in Loop: Header=BB0_89 Depth=1
	addi	r10, r11, -97
	andv	r10, 255
	cmprv	r10, 25
	jmpugtrel	.LBB0_94
# %bb.93:                               # %if.then5.i.i642
                                        #   in Loop: Header=BB0_89 Depth=1
	addi	r10, r11, -71
	jmprel	.LBB0_101
.LBB0_94:                               # %if.end7.i.i616
                                        #   in Loop: Header=BB0_89 Depth=1
	addi	r10, r11, -48
	andv	r10, 255
	cmprv	r10, 9
	jmpugtrel	.LBB0_96
# %bb.95:                               # %if.then11.i.i640
                                        #   in Loop: Header=BB0_89 Depth=1
	addi	r10, r11, 4
	jmprel	.LBB0_101
.LBB0_96:                               # %if.end14.i.i618
                                        #   in Loop: Header=BB0_89 Depth=1
	cmprv	r11, 43
	jmperel	.LBB0_97
# %bb.98:                               # %if.end14.i.i618
                                        #   in Loop: Header=BB0_89 Depth=1
	cmprv	r11, 47
	jmpnerel	.LBB0_105
# %bb.100:                              # %if.end7.fold.split.i619
                                        #   in Loop: Header=BB0_89 Depth=1
	setr	r10, 63
	jmprel	.LBB0_101
.LBB0_97:                               #   in Loop: Header=BB0_89 Depth=1
	setr	r10, 62
.LBB0_101:                              # %if.end7.i620
                                        #   in Loop: Header=BB0_89 Depth=1
	shlv	r14, 6
	orr	r14, r10, r14
	copy	r11, r13
	sextw	r11
	cmprv	r11, 2
	jmpltrel	.LBB0_102
# %bb.103:                              # %if.then10.i631
                                        #   in Loop: Header=BB0_89 Depth=1
	leapc	r6, g_fail
	leapc	r2, .L.str.42
	copy	r11, r12
	sextw	r11
	cmprv	r11, 63
	jmpgtrel	.LBB0_112
# %bb.104:                              # %if.end14.i633
                                        #   in Loop: Header=BB0_89 Depth=1
	addr	r11, r5, r11
	copy	r10, r14
	zextw	r10
	addi	r13, r13, -2
	copy	r9, r13
	zextw	r9
	shrr	r10, r10, r9
	memset8	r10, r11
	incr	r12
.LBB0_105:                              # %for.inc.i626
                                        #   in Loop: Header=BB0_89 Depth=1
	incr	r4
	jmprel	.LBB0_89
.LBB0_106:                              # %base64_decode.exit644
	leapc	r6, g_fail
	leapc	r2, .L.str.42
	zextw	r12
	cmprv	r12, 16
	jmpnerel	.LBB0_112
# %bb.107:                              # %for.body.preheader
	setr	r12, 0
.LBB0_113:                              # %for.body
                                        # =>This Inner Loop Header: Depth=1
	leapc	r14, main.bin
	addr	r14, r14, r12
	memget8	r14, r14
	andv	r14, 255
	addr	r13, r5, r12
	memget8	r13, r13
	andv	r13, 255
	cmpeqr	r14, r13, r14
	cmprv	r12, 14
	jmpugtrel	.LBB0_108
# %bb.114:                              # %for.body
                                        #   in Loop: Header=BB0_113 Depth=1
	incr	r12
	copy	r13, r14
	andv	r13, 1
	cmprv	r13, 0
	jmpnerel	.LBB0_113
.LBB0_108:                              # %for.cond.cleanup
	leapc	r6, g_fail
	leapc	r12, g_pass
	cmprv	r14, 0
	jmpe	.LBB0_110
# %bb.109:                              # %for.cond.cleanup
	copy	r6, r12
.LBB0_110:                              # %for.cond.cleanup
	leapc	r2, .L.str.42
	leapc	r12, .L.str.41
	cmprv	r14, 0
	jmpe	.LBB0_112
# %bb.111:                              # %for.cond.cleanup
	copy	r2, r12
.LBB0_112:                              # %.thread
	leapc	r0, .L.str.40
	leapc	r1, .L.str.35
	callrel	printf
	memget32	r12, r6
	incr	r12
	memset32	r12, r6
	leapc	r12, g_pass
	memget32	r1, r12
	leapc	r4, g_fail
	memget32	r2, r4
	leapc	r0, .L.str.36
	callrel	printf
	memget32	r12, r4
	ldidx64	r7, r15, -32                    # 8-byte Folded Reload
	ldidx64	r6, r15, -24                    # 8-byte Folded Reload
	ldidx64	r5, r15, -16                    # 8-byte Folded Reload
	ldidx64	r4, r15, -8                     # 8-byte Folded Reload
	setsp	r15
	pop	r15
	ret
.Lfunc_end0:
	.size	main, .Lfunc_end0-main
                                        # -- End function
	.section	.text.sha256,"ax",@progbits
	.p2align	2                               # -- Begin function sha256
	.type	sha256,@function
sha256:                                 # @sha256
# %bb.0:                                # %entry
	push	r15
	getsp	r15
	addsp	-680
	stidx64	r4, r15, -8                     # 8-byte Folded Spill
	stidx64	r5, r15, -16                    # 8-byte Folded Spill
	stidx64	r6, r15, -24                    # 8-byte Folded Spill
	stidx64	r7, r15, -32                    # 8-byte Folded Spill
	stidx64	r2, r15, -680                   # 8-byte Folded Spill
	addi	r4, r1, 72
	andv	r4, 192
	copy	r12, r1
	zextw	r12
	cmprv	r12, 0
	addi	r5, r15, -544
	jmperel	.LBB1_1
# %bb.2:                                # %for.body.preheader
	copy	r14, r1
	andv	r14, 3
	cmprv	r12, 4
	jmpultrel	.LBB1_3
	jmprel	.LBB1_4
.LBB1_3:
	setr	r13, 0
	jmprel	.LBB1_7
.LBB1_1:
	setr	r12, 0
	jmprel	.LBB1_9
.LBB1_4:                                # %for.body.preheader.new
	setr	r13, 0
	copy	r11, r0
	incr	r11
	copy	r10, r5
	incr	r10
	copy	r9, r12
	andv	r9, 60
.LBB1_12:                               # %for.body
                                        # =>This Inner Loop Header: Depth=1
	addr	r8, r5, r13
	addr	r2, r11, r13
	ldidx8	r3, r2, -1
	memset8	r3, r8
	addr	r8, r10, r13
	memget8	r3, r2
	memset8	r3, r8
	ldidx8	r3, r2, 1
	stidx8	r3, r8, 1
	ldidx8	r2, r2, 2
	stidx8	r2, r8, 2
	addi	r13, r13, 4
	cmprr	r9, r13
	jmperel	.LBB1_5
	jmprel	.LBB1_12
.LBB1_5:                                # %for.cond.cleanup.loopexit.unr-lcssa
	cmprv	r14, 0
	jmperel	.LBB1_9
.LBB1_7:                                # %for.body.epil.preheader
	addr	r11, r0, r13
	addr	r13, r5, r13
.LBB1_8:                                # %for.body.epil
                                        # =>This Inner Loop Header: Depth=1
	memget8	r10, r11
	memset8	r10, r13
	incr	r11
	incr	r13
	decr	r14
	jmpnzrel	.LBB1_8
.LBB1_9:                                # %for.cond.cleanup
	addr	r14, r5, r12
	setr	r13, 128
	memset8	r13, r14
	addi	r14, r4, -8
	copy	r13, r14
	zextw	r13
	copy	r11, r1
	incr	r11
	zextw	r11
	cmprr	r11, r13
	jmpultrel	.LBB1_11
	jmprel	.LBB1_13
.LBB1_11:                               # %for.body11.preheader
	addi	r13, r4, -64
	zextw	r13
	subr	r11, r12, r13
	addr	r13, r12, r5
	incr	r13
	addi	r11, r11, -55
	setr	r10, 0
.LBB1_14:                               # %for.body11
                                        # =>This Inner Loop Header: Depth=1
	memset8	r10, r13
	incr	r11
	cmpeqr	r9, r11, r10
	incr	r13
	andv	r9, 1
	cmprv	r9, 0
	jmpnerel	.LBB1_13
	jmprel	.LBB1_14
.LBB1_13:                               # %for.body60.lr.ph
	addr	r14, r5, r14
	setr	r13, 0
	memset8	r13, r14
	addr	r14, r5, r4
	shlv	r1, 3
	stidx8	r1, r14, -1
	shrv	r12, 5
	stidx8	r12, r14, -2
	stidx8	r13, r14, -3
	stidx8	r13, r14, -4
	stidx8	r13, r14, -5
	stidx8	r13, r14, -6
	stidx8	r13, r14, -7
	setr	r6, 1779033703
	setr64	r12, -1150833019, 0
	stidx64	r12, r15, -576                  # 8-byte Folded Spill
	setr	r12, 1013904242
	stidx64	r12, r15, -584                  # 8-byte Folded Spill
	setr64	r12, -1521486534, 0
	stidx64	r12, r15, -592                  # 8-byte Folded Spill
	setr	r12, 1359893119
	stidx64	r12, r15, -600                  # 8-byte Folded Spill
	setr64	r12, -1694144372, 0
	stidx64	r12, r15, -608                  # 8-byte Folded Spill
	setr	r12, 528734635
	stidx64	r12, r15, -616                  # 8-byte Folded Spill
	setr	r12, 1541459225
	stidx64	r12, r15, -624                  # 8-byte Folded Spill
	addi	r12, r15, -288
	stidx64	r12, r15, -568                  # 8-byte Folded Spill
	addi	r12, r12, 72
	stidx64	r12, r15, -672                  # 8-byte Folded Spill
	stidx64	r13, r15, -664                  # 8-byte Folded Spill
	stidx64	r4, r15, -648                   # 8-byte Folded Spill
	stidx64	r5, r15, -656                   # 8-byte Folded Spill
.LBB1_16:                               # %for.body60
                                        # =>This Loop Header: Depth=1
                                        #     Child Loop BB1_17 Depth 2
                                        #     Child Loop BB1_19 Depth 2
	stidx64	r13, r15, -640                  # 8-byte Folded Spill
	addr	r12, r5, r13
	ldidx8	r14, r12, 5
	shlv	r14, 8
	ldidx8	r13, r12, 4
	orr	r10, r14, r13
	ldidx8	r14, r12, 6
	shlv	r14, 16
	ldidx8	r13, r12, 7
	shlv	r13, 24
	orr	r8, r13, r14
	ldidx8	r14, r12, 9
	shlv	r14, 8
	ldidx8	r13, r12, 8
	orr	r13, r14, r13
	ldidx8	r14, r12, 10
	shlv	r14, 16
	ldidx8	r11, r12, 11
	shlv	r11, 24
	orr	r1, r11, r14
	ldidx8	r14, r12, 17
	shlv	r14, 8
	ldidx8	r11, r12, 16
	orr	r5, r14, r11
	ldidx8	r11, r12, 21
	shlv	r11, 8
	ldidx8	r9, r12, 20
	orr	r14, r11, r9
	stidx64	r14, r15, -552                  # 8-byte Folded Spill
	ldidx8	r9, r12, 25
	shlv	r9, 8
	ldidx8	r0, r12, 24
	orr	r14, r9, r0
	stidx64	r14, r15, -560                  # 8-byte Folded Spill
	ldidx8	r0, r12, 29
	shlv	r0, 8
	ldidx8	r2, r12, 28
	orr	r0, r0, r2
	orr	r13, r1, r13
	orr	r10, r8, r10
	ldidx8	r8, r12, 13
	shlv	r8, 8
	ldidx8	r1, r12, 12
	orr	r1, r8, r1
	ldidx8	r8, r12, 14
	shlv	r8, 16
	ldidx8	r2, r12, 15
	shlv	r2, 24
	orr	r2, r2, r8
	ldidx8	r8, r12, 18
	shlv	r8, 16
	ldidx8	r3, r12, 19
	shlv	r3, 24
	orr	r8, r3, r8
	ldidx8	r3, r12, 22
	shlv	r3, 16
	ldidx8	r4, r12, 23
	shlv	r4, 24
	orr	r4, r4, r3
	ldidx8	r3, r12, 26
	shlv	r3, 16
	ldidx8	r7, r12, 27
	shlv	r7, 24
	orr	r7, r7, r3
	ldidx8	r3, r12, 30
	shlv	r3, 16
	ldidx8	r14, r12, 31
	shlv	r14, 24
	orr	r14, r14, r3
	ldidx8	r3, r12, 33
	shlv	r3, 8
	ldidx8	r11, r12, 32
	orr	r11, r3, r11
	ldidx8	r3, r12, 34
	shlv	r3, 16
	ldidx8	r9, r12, 35
	shlv	r9, 24
	orr	r9, r9, r3
	orr	r3, r9, r11
	orr	r0, r14, r0
	ldidx64	r14, r15, -560                  # 8-byte Folded Reload
	orr	r9, r7, r14
	ldidx64	r14, r15, -552                  # 8-byte Folded Reload
	orr	r14, r4, r14
	orr	r11, r8, r5
	orr	r8, r2, r1
	bswap	r10
	shrv	r10, 32
	ldidx64	r5, r15, -568                   # 8-byte Folded Reload
	stidx32	r10, r5, 4
	bswap	r13
	shrv	r13, 32
	stidx32	r13, r5, 8
	bswap	r8
	shrv	r8, 32
	stidx32	r8, r5, 12
	bswap	r11
	shrv	r11, 32
	stidx32	r11, r5, 16
	bswap	r14
	shrv	r14, 32
	stidx32	r14, r5, 20
	bswap	r9
	shrv	r9, 32
	stidx32	r9, r5, 24
	bswap	r0
	shrv	r0, 32
	stidx32	r0, r5, 28
	bswap	r3
	shrv	r3, 32
	stidx32	r3, r5, 32
	ldidx8	r14, r12, 1
	shlv	r14, 8
	memget8	r13, r12
	orr	r14, r14, r13
	ldidx8	r13, r12, 2
	shlv	r13, 16
	ldidx8	r11, r12, 3
	shlv	r11, 24
	orr	r13, r11, r13
	orr	r14, r13, r14
	bswap	r14
	shrv	r14, 32
	stidx32	r14, r15, -288
	ldidx8	r14, r12, 37
	shlv	r14, 8
	ldidx8	r13, r12, 36
	orr	r13, r14, r13
	ldidx8	r14, r12, 38
	shlv	r14, 16
	ldidx8	r11, r12, 39
	shlv	r11, 24
	orr	r10, r11, r14
	ldidx8	r14, r12, 41
	shlv	r14, 8
	ldidx8	r11, r12, 40
	orr	r9, r14, r11
	ldidx8	r14, r12, 42
	shlv	r14, 16
	ldidx8	r11, r12, 43
	shlv	r11, 24
	orr	r8, r11, r14
	ldidx8	r14, r12, 45
	shlv	r14, 8
	ldidx8	r11, r12, 44
	orr	r11, r14, r11
	ldidx8	r14, r12, 46
	shlv	r14, 16
	ldidx8	r0, r12, 47
	shlv	r0, 24
	orr	r0, r0, r14
	ldidx8	r14, r12, 49
	shlv	r14, 8
	ldidx8	r1, r12, 48
	orr	r14, r14, r1
	ldidx8	r1, r12, 50
	shlv	r1, 16
	ldidx8	r2, r12, 51
	shlv	r2, 24
	orr	r1, r2, r1
	orr	r14, r1, r14
	orr	r11, r0, r11
	orr	r9, r8, r9
	orr	r13, r10, r13
	ldidx8	r10, r12, 53
	shlv	r10, 8
	ldidx8	r8, r12, 52
	orr	r8, r10, r8
	ldidx8	r10, r12, 54
	shlv	r10, 16
	ldidx8	r0, r12, 55
	shlv	r0, 24
	orr	r0, r0, r10
	ldidx8	r10, r12, 57
	shlv	r10, 8
	ldidx8	r1, r12, 56
	orr	r10, r10, r1
	ldidx8	r1, r12, 58
	shlv	r1, 16
	ldidx8	r2, r12, 59
	shlv	r2, 24
	orr	r1, r2, r1
	orr	r10, r1, r10
	orr	r8, r0, r8
	bswap	r13
	shrv	r13, 32
	stidx32	r13, r5, 36
	bswap	r9
	shrv	r9, 32
	stidx32	r9, r5, 40
	bswap	r11
	shrv	r11, 32
	stidx32	r11, r5, 44
	bswap	r14
	shrv	r14, 32
	stidx32	r14, r5, 48
	bswap	r8
	shrv	r8, 32
	stidx32	r8, r5, 52
	bswap	r10
	shrv	r10, 32
	stidx32	r10, r5, 56
	ldidx8	r14, r12, 61
	shlv	r14, 8
	ldidx8	r13, r12, 60
	orr	r14, r14, r13
	ldidx8	r13, r12, 62
	shlv	r13, 16
	ldidx8	r12, r12, 63
	shlv	r12, 24
	orr	r12, r12, r13
	orr	r12, r12, r14
	bswap	r12
	shrv	r12, 32
	stidx32	r12, r5, 60
	ldidx64	r12, r15, -664                  # 8-byte Folded Reload
	ldidx64	r7, r15, -672                   # 8-byte Folded Reload
.LBB1_17:                               # %for.body26.i
                                        #   Parent Loop BB1_16 Depth=1
                                        # =>  This Inner Loop Header: Depth=2
	addr	r13, r7, r12
	ldidx32	r11, r13, -16
	copy	r14, r11
	shrv	r14, 19
	copy	r10, r11
	shlv	r10, 13
	orr	r14, r10, r14
	copy	r10, r11
	shrv	r10, 17
	copy	r9, r11
	shlv	r9, 15
	orr	r9, r9, r10
	ldidx32	r10, r13, -68
	copy	r8, r10
	shrv	r8, 18
	copy	r0, r10
	shlv	r0, 14
	orr	r8, r0, r8
	copy	r0, r10
	shrv	r0, 7
	copy	r1, r10
	shlv	r1, 25
	orr	r0, r1, r0
	xorr	r8, r0, r8
	xorr	r9, r9, r14
	ldidx32	r14, r13, -64
	copy	r0, r14
	shrv	r0, 18
	copy	r1, r14
	shlv	r1, 14
	orr	r0, r1, r0
	copy	r1, r14
	shrv	r1, 7
	copy	r2, r14
	shlv	r2, 25
	orr	r1, r2, r1
	xorr	r0, r1, r0
	copy	r1, r14
	shrv	r1, 3
	xorr	r0, r0, r1
	shrv	r11, 10
	xorr	r11, r9, r11
	addr	r9, r0, r10
	shrv	r10, 3
	xorr	r10, r8, r10
	addr	r8, r5, r12
	memget32	r8, r8
	addr	r10, r10, r8
	ldidx32	r8, r13, -36
	addr	r10, r10, r8
	addr	r11, r10, r11
	ldidx32	r10, r13, -12
	copy	r8, r10
	shrv	r8, 19
	copy	r0, r10
	shlv	r0, 13
	orr	r8, r0, r8
	copy	r0, r10
	shrv	r0, 17
	copy	r1, r10
	shlv	r1, 15
	orr	r0, r1, r0
	xorr	r8, r0, r8
	shrv	r10, 10
	xorr	r8, r8, r10
	ldidx32	r10, r13, -32
	addr	r9, r9, r10
	copy	r0, r11
	shlv	r0, 13
	copy	r10, r11
	zextw	r10
	copy	r1, r10
	shrv	r1, 19
	orr	r0, r0, r1
	copy	r1, r11
	shlv	r1, 15
	copy	r2, r10
	shrv	r2, 17
	orr	r1, r1, r2
	addr	r9, r9, r8
	ldidx32	r8, r13, -60
	copy	r2, r8
	shrv	r2, 18
	copy	r3, r8
	shlv	r3, 14
	orr	r2, r3, r2
	copy	r3, r8
	shrv	r3, 7
	copy	r4, r8
	shlv	r4, 25
	orr	r3, r4, r3
	xorr	r2, r3, r2
	shrv	r8, 3
	xorr	r8, r2, r8
	xorr	r0, r1, r0
	stidx32	r11, r13, -8
	stidx32	r9, r13, -4
	addr	r14, r8, r14
	shrv	r10, 10
	xorr	r11, r0, r10
	ldidx32	r10, r13, -28
	addr	r14, r14, r10
	addr	r14, r14, r11
	memset32	r14, r13
	addi	r12, r12, 12
	cmprv	r12, 192
	jmpnerel	.LBB1_17
# %bb.18:                               # %for.body78.i.preheader
                                        #   in Loop: Header=BB1_16 Depth=1
	setr	r2, 0
	stidx64	r6, r15, -632                   # 8-byte Folded Spill
	copy	r8, r6
	ldidx64	r9, r15, -576                   # 8-byte Folded Reload
	ldidx64	r0, r15, -584                   # 8-byte Folded Reload
	ldidx64	r14, r15, -592                  # 8-byte Folded Reload
	ldidx64	r10, r15, -624                  # 8-byte Folded Reload
	ldidx64	r1, r15, -616                   # 8-byte Folded Reload
	ldidx64	r4, r15, -608                   # 8-byte Folded Reload
	ldidx64	r11, r15, -600                  # 8-byte Folded Reload
.LBB1_19:                               # %for.body78.i
                                        #   Parent Loop BB1_16 Depth=1
                                        # =>  This Inner Loop Header: Depth=2
	copy	r3, r11
	copy	r12, r4
	stidx64	r12, r15, -552                  # 8-byte Folded Spill
	copy	r13, r14
	copy	r11, r0
	copy	r14, r9
	copy	r0, r8
	copy	r9, r3
	shlv	r9, 21
	copy	r8, r3
	zextw	r8
	copy	r4, r8
	shrv	r4, 11
	orr	r9, r9, r4
	copy	r4, r3
	shlv	r4, 26
	copy	r7, r8
	shrv	r7, 6
	orr	r4, r4, r7
	xorr	r9, r4, r9
	shrv	r8, 25
	copy	r4, r3
	shlv	r4, 7
	orr	r8, r4, r8
	xorr	r9, r9, r8
	leapc	r8, K
	addr	r8, r8, r2
	memget32	r4, r8
	addr	r10, r4, r10
	xorr	r4, r12, r1
	andr	r4, r4, r3
	xorr	r4, r4, r1
	addr	r4, r10, r4
	ldidx64	r12, r15, -568                  # 8-byte Folded Reload
	addr	r10, r12, r2
	memget32	r7, r10
	addr	r4, r4, r7
	addr	r4, r4, r9
	stidx64	r11, r15, -560                  # 8-byte Folded Spill
	xorr	r9, r11, r14
	andr	r9, r9, r0
	andr	r7, r11, r14
	xorr	r9, r9, r7
	copy	r7, r0
	shlv	r7, 19
	copy	r5, r0
	zextw	r5
	copy	r12, r5
	shrv	r12, 13
	orr	r12, r7, r12
	copy	r7, r0
	shlv	r7, 30
	copy	r6, r5
	shrv	r6, 2
	orr	r6, r7, r6
	xorr	r12, r6, r12
	shrv	r5, 22
	copy	r6, r0
	shlv	r6, 10
	orr	r5, r6, r5
	xorr	r12, r12, r5
	addr	r12, r9, r12
	addr	r9, r12, r4
	addr	r4, r4, r13
	ldidx32	r12, r8, 4
	addr	r1, r12, r1
	xorr	r8, r14, r0
	andr	r11, r14, r0
	copy	r12, r9
	shlv	r12, 19
	copy	r5, r9
	zextw	r5
	copy	r6, r5
	shrv	r6, 13
	orr	r12, r12, r6
	copy	r6, r9
	shlv	r6, 30
	copy	r7, r5
	shrv	r7, 2
	orr	r6, r6, r7
	xorr	r12, r6, r12
	shrv	r5, 22
	copy	r6, r9
	shlv	r6, 10
	orr	r5, r6, r5
	xorr	r12, r12, r5
	copy	r5, r4
	shlv	r5, 21
	copy	r13, r4
	zextw	r13
	copy	r7, r13
	shrv	r7, 11
	orr	r5, r5, r7
	copy	r7, r4
	shlv	r7, 26
	copy	r6, r13
	shrv	r6, 6
	orr	r6, r7, r6
	xorr	r5, r6, r5
	shrv	r13, 25
	copy	r6, r4
	shlv	r6, 7
	orr	r13, r6, r13
	andr	r8, r8, r9
	xorr	r11, r8, r11
	addr	r12, r11, r12
	xorr	r13, r5, r13
	ldidx64	r5, r15, -552                   # 8-byte Folded Reload
	xorr	r11, r3, r5
	andr	r11, r11, r4
	xorr	r11, r11, r5
	addr	r11, r1, r11
	ldidx32	r10, r10, 4
	addr	r11, r11, r10
	addr	r13, r11, r13
	addr	r8, r12, r13
	ldidx64	r12, r15, -560                  # 8-byte Folded Reload
	addr	r11, r13, r12
	copy	r13, r5
	addi	r2, r2, 8
	cmprv	r2, 256
	copy	r10, r13
	copy	r1, r3
	jmpnerel	.LBB1_19
# %bb.20:                               # %sha256_block.exit
                                        #   in Loop: Header=BB1_16 Depth=1
	ldidx64	r12, r15, -624                  # 8-byte Folded Reload
	addr	r12, r13, r12
	stidx64	r12, r15, -624                  # 8-byte Folded Spill
	ldidx64	r12, r15, -616                  # 8-byte Folded Reload
	addr	r12, r3, r12
	stidx64	r12, r15, -616                  # 8-byte Folded Spill
	ldidx64	r12, r15, -608                  # 8-byte Folded Reload
	addr	r12, r4, r12
	stidx64	r12, r15, -608                  # 8-byte Folded Spill
	ldidx64	r12, r15, -600                  # 8-byte Folded Reload
	addr	r12, r11, r12
	stidx64	r12, r15, -600                  # 8-byte Folded Spill
	ldidx64	r12, r15, -592                  # 8-byte Folded Reload
	addr	r12, r14, r12
	stidx64	r12, r15, -592                  # 8-byte Folded Spill
	ldidx64	r12, r15, -584                  # 8-byte Folded Reload
	addr	r12, r0, r12
	stidx64	r12, r15, -584                  # 8-byte Folded Spill
	ldidx64	r12, r15, -576                  # 8-byte Folded Reload
	addr	r12, r9, r12
	stidx64	r12, r15, -576                  # 8-byte Folded Spill
	ldidx64	r6, r15, -632                   # 8-byte Folded Reload
	addr	r6, r8, r6
	ldidx64	r13, r15, -640                  # 8-byte Folded Reload
	addi	r13, r13, 64
	ldidx64	r4, r15, -648                   # 8-byte Folded Reload
	cmprr	r13, r4
	ldidx64	r5, r15, -656                   # 8-byte Folded Reload
	jmpultrel	.LBB1_16
# %bb.15:                               # %for.cond66.preheader
	ldidx64	r13, r15, -624                  # 8-byte Folded Reload
	copy	r12, r13
	shrv	r12, 8
	ldidx64	r14, r15, -680                  # 8-byte Folded Reload
	stidx8	r12, r14, 30
	copy	r12, r13
	shrv	r12, 16
	stidx8	r12, r14, 29
	stidx8	r13, r14, 31
	shrv	r13, 24
	stidx8	r13, r14, 28
	ldidx64	r13, r15, -616                  # 8-byte Folded Reload
	copy	r12, r13
	shrv	r12, 8
	stidx8	r12, r14, 26
	copy	r12, r13
	shrv	r12, 16
	stidx8	r12, r14, 25
	stidx8	r13, r14, 27
	shrv	r13, 24
	stidx8	r13, r14, 24
	ldidx64	r13, r15, -608                  # 8-byte Folded Reload
	copy	r12, r13
	shrv	r12, 8
	stidx8	r12, r14, 22
	copy	r12, r13
	shrv	r12, 16
	stidx8	r12, r14, 21
	stidx8	r13, r14, 23
	shrv	r13, 24
	stidx8	r13, r14, 20
	ldidx64	r13, r15, -600                  # 8-byte Folded Reload
	copy	r12, r13
	shrv	r12, 8
	stidx8	r12, r14, 18
	copy	r12, r13
	shrv	r12, 16
	stidx8	r12, r14, 17
	stidx8	r13, r14, 19
	shrv	r13, 24
	stidx8	r13, r14, 16
	ldidx64	r13, r15, -592                  # 8-byte Folded Reload
	copy	r12, r13
	shrv	r12, 8
	stidx8	r12, r14, 14
	copy	r12, r13
	shrv	r12, 16
	stidx8	r12, r14, 13
	stidx8	r13, r14, 15
	shrv	r13, 24
	stidx8	r13, r14, 12
	ldidx64	r13, r15, -584                  # 8-byte Folded Reload
	copy	r12, r13
	shrv	r12, 8
	stidx8	r12, r14, 10
	copy	r12, r13
	shrv	r12, 16
	stidx8	r12, r14, 9
	stidx8	r13, r14, 11
	shrv	r13, 24
	stidx8	r13, r14, 8
	ldidx64	r13, r15, -576                  # 8-byte Folded Reload
	copy	r12, r13
	shrv	r12, 8
	stidx8	r12, r14, 6
	copy	r12, r13
	shrv	r12, 16
	stidx8	r12, r14, 5
	stidx8	r13, r14, 7
	shrv	r13, 24
	stidx8	r13, r14, 4
	copy	r12, r6
	shrv	r12, 8
	stidx8	r12, r14, 2
	copy	r12, r6
	shrv	r12, 16
	stidx8	r12, r14, 1
	stidx8	r6, r14, 3
	shrv	r6, 24
	memset8	r6, r14
	ldidx64	r7, r15, -32                    # 8-byte Folded Reload
	ldidx64	r6, r15, -24                    # 8-byte Folded Reload
	ldidx64	r5, r15, -16                    # 8-byte Folded Reload
	ldidx64	r4, r15, -8                     # 8-byte Folded Reload
	setsp	r15
	pop	r15
	ret
.Lfunc_end1:
	.size	sha256, .Lfunc_end1-sha256
                                        # -- End function
	.type	g_pass,@object                  # @g_pass
	.section	.bss.g_pass,"aw",@nobits
	.p2align	2, 0x0
g_pass:
	.long	0                               # 0x0
	.size	g_pass, 4

	.type	g_fail,@object                  # @g_fail
	.section	.bss.g_fail,"aw",@nobits
	.p2align	2, 0x0
g_fail:
	.long	0                               # 0x0
	.size	g_fail, 4

	.type	.L.str,@object                  # @.str
	.section	.rodata.str1.1,"aMS",@progbits,1
.L.str:
	.asciz	"=== Crypto / Hash ===\n"
	.size	.L.str, 23

	.type	.L.str.1,@object                # @.str.1
.L.str.1:
	.asciz	"CRC32 empty:      "
	.size	.L.str.1, 19

	.type	.L.str.2,@object                # @.str.2
.L.str.2:
	.asciz	"CRC32 'a':        "
	.size	.L.str.2, 19

	.type	.L.str.3,@object                # @.str.3
.L.str.3:
	.asciz	"CRC32 'abc':      "
	.size	.L.str.3, 19

	.type	.L.str.4,@object                # @.str.4
.L.str.4:
	.asciz	"CRC32 '12...9':   "
	.size	.L.str.4, 19

	.type	.L.str.5,@object                # @.str.5
.L.str.5:
	.asciz	"CRC32 'fox...':   "
	.size	.L.str.5, 19

	.type	.L.str.6,@object                # @.str.6
.L.str.6:
	.zero	1
	.size	.L.str.6, 1

	.type	.L.str.7,@object                # @.str.7
.L.str.7:
	.asciz	"SHA256 empty:     "
	.size	.L.str.7, 19

	.type	.L.str.8,@object                # @.str.8
.L.str.8:
	.asciz	"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
	.size	.L.str.8, 65

	.type	.L.str.9,@object                # @.str.9
.L.str.9:
	.asciz	"abc"
	.size	.L.str.9, 4

	.type	.L.str.10,@object               # @.str.10
.L.str.10:
	.asciz	"SHA256 'abc':     "
	.size	.L.str.10, 19

	.type	.L.str.11,@object               # @.str.11
.L.str.11:
	.asciz	"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
	.size	.L.str.11, 65

	.type	.L.str.12,@object               # @.str.12
.L.str.12:
	.asciz	"a"
	.size	.L.str.12, 2

	.type	.L.str.13,@object               # @.str.13
.L.str.13:
	.asciz	"SHA256 'a':       "
	.size	.L.str.13, 19

	.type	.L.str.14,@object               # @.str.14
.L.str.14:
	.asciz	"ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb"
	.size	.L.str.14, 65

	.type	.L.str.15,@object               # @.str.15
.L.str.15:
	.asciz	"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"
	.size	.L.str.15, 57

	.type	.L.str.16,@object               # @.str.16
.L.str.16:
	.asciz	"SHA256 56-byte:   "
	.size	.L.str.16, 19

	.type	.L.str.17,@object               # @.str.17
.L.str.17:
	.asciz	"248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"
	.size	.L.str.17, 65

	.type	.L.str.18,@object               # @.str.18
.L.str.18:
	.asciz	"B64 enc empty:    "
	.size	.L.str.18, 19

	.type	.L.str.20,@object               # @.str.20
.L.str.20:
	.asciz	"B64 enc 'f':      "
	.size	.L.str.20, 19

	.type	.L.str.21,@object               # @.str.21
.L.str.21:
	.asciz	"Zg=="
	.size	.L.str.21, 5

	.type	.L.str.23,@object               # @.str.23
.L.str.23:
	.asciz	"B64 enc 'fo':     "
	.size	.L.str.23, 19

	.type	.L.str.24,@object               # @.str.24
.L.str.24:
	.asciz	"Zm8="
	.size	.L.str.24, 5

	.type	.L.str.26,@object               # @.str.26
.L.str.26:
	.asciz	"B64 enc 'foo':    "
	.size	.L.str.26, 19

	.type	.L.str.27,@object               # @.str.27
.L.str.27:
	.asciz	"Zm9v"
	.size	.L.str.27, 5

	.type	.L.str.29,@object               # @.str.29
.L.str.29:
	.asciz	"B64 enc 'foob':   "
	.size	.L.str.29, 19

	.type	.L.str.30,@object               # @.str.30
.L.str.30:
	.asciz	"Zm9vYg=="
	.size	.L.str.30, 9

	.type	.L.str.32,@object               # @.str.32
.L.str.32:
	.asciz	"B64 enc 'foobar': "
	.size	.L.str.32, 19

	.type	.L.str.33,@object               # @.str.33
.L.str.33:
	.asciz	"Zm9vYmFy"
	.size	.L.str.33, 9

	.type	.L.str.34,@object               # @.str.34
.L.str.34:
	.asciz	"B64 dec 'foobar': "
	.size	.L.str.34, 19

	.type	main.bin,@object                # @main.bin
	.section	.rodata.cst16,"aM",@progbits,16
main.bin:
	.ascii	"\000\001\002\177\200\376\377\253\315\357\0224Vx\232\274"
	.size	main.bin, 16

	.type	.L.str.35,@object               # @.str.35
	.section	.rodata.str1.1,"aMS",@progbits,1
.L.str.35:
	.asciz	"B64 bin round:    "
	.size	.L.str.35, 19

	.type	.L.str.36,@object               # @.str.36
.L.str.36:
	.asciz	"\nResults: %d pass, %d fail\n"
	.size	.L.str.36, 28

	.type	.L.str.37,@object               # @.str.37
.L.str.37:
	.asciz	"%s"
	.size	.L.str.37, 3

	.type	.L.str.38,@object               # @.str.38
.L.str.38:
	.asciz	"PASS\n"
	.size	.L.str.38, 6

	.type	K,@object                       # @K
	.section	.rodata.K,"a",@progbits
	.p2align	2, 0x0
K:
	.long	1116352408                      # 0x428a2f98
	.long	1899447441                      # 0x71374491
	.long	3049323471                      # 0xb5c0fbcf
	.long	3921009573                      # 0xe9b5dba5
	.long	961987163                       # 0x3956c25b
	.long	1508970993                      # 0x59f111f1
	.long	2453635748                      # 0x923f82a4
	.long	2870763221                      # 0xab1c5ed5
	.long	3624381080                      # 0xd807aa98
	.long	310598401                       # 0x12835b01
	.long	607225278                       # 0x243185be
	.long	1426881987                      # 0x550c7dc3
	.long	1925078388                      # 0x72be5d74
	.long	2162078206                      # 0x80deb1fe
	.long	2614888103                      # 0x9bdc06a7
	.long	3248222580                      # 0xc19bf174
	.long	3835390401                      # 0xe49b69c1
	.long	4022224774                      # 0xefbe4786
	.long	264347078                       # 0xfc19dc6
	.long	604807628                       # 0x240ca1cc
	.long	770255983                       # 0x2de92c6f
	.long	1249150122                      # 0x4a7484aa
	.long	1555081692                      # 0x5cb0a9dc
	.long	1996064986                      # 0x76f988da
	.long	2554220882                      # 0x983e5152
	.long	2821834349                      # 0xa831c66d
	.long	2952996808                      # 0xb00327c8
	.long	3210313671                      # 0xbf597fc7
	.long	3336571891                      # 0xc6e00bf3
	.long	3584528711                      # 0xd5a79147
	.long	113926993                       # 0x6ca6351
	.long	338241895                       # 0x14292967
	.long	666307205                       # 0x27b70a85
	.long	773529912                       # 0x2e1b2138
	.long	1294757372                      # 0x4d2c6dfc
	.long	1396182291                      # 0x53380d13
	.long	1695183700                      # 0x650a7354
	.long	1986661051                      # 0x766a0abb
	.long	2177026350                      # 0x81c2c92e
	.long	2456956037                      # 0x92722c85
	.long	2730485921                      # 0xa2bfe8a1
	.long	2820302411                      # 0xa81a664b
	.long	3259730800                      # 0xc24b8b70
	.long	3345764771                      # 0xc76c51a3
	.long	3516065817                      # 0xd192e819
	.long	3600352804                      # 0xd6990624
	.long	4094571909                      # 0xf40e3585
	.long	275423344                       # 0x106aa070
	.long	430227734                       # 0x19a4c116
	.long	506948616                       # 0x1e376c08
	.long	659060556                       # 0x2748774c
	.long	883997877                       # 0x34b0bcb5
	.long	958139571                       # 0x391c0cb3
	.long	1322822218                      # 0x4ed8aa4a
	.long	1537002063                      # 0x5b9cca4f
	.long	1747873779                      # 0x682e6ff3
	.long	1955562222                      # 0x748f82ee
	.long	2024104815                      # 0x78a5636f
	.long	2227730452                      # 0x84c87814
	.long	2361852424                      # 0x8cc70208
	.long	2428436474                      # 0x90befffa
	.long	2756734187                      # 0xa4506ceb
	.long	3204031479                      # 0xbef9a3f7
	.long	3329325298                      # 0xc67178f2
	.size	K, 256

	.type	.L.str.40,@object               # @.str.40
	.section	.rodata.str1.1,"aMS",@progbits,1
.L.str.40:
	.asciz	"%s%s\n"
	.size	.L.str.40, 6

	.type	.L.str.41,@object               # @.str.41
.L.str.41:
	.asciz	"PASS"
	.size	.L.str.41, 5

	.type	.L.str.42,@object               # @.str.42
.L.str.42:
	.asciz	"FAIL"
	.size	.L.str.42, 5

	.type	hexcmp.hexd,@object             # @hexcmp.hexd
hexcmp.hexd:
	.asciz	"0123456789abcdef"
	.size	hexcmp.hexd, 17

	.ident	"clang version 23.0.0git (https://github.com/grahamjonesgs/klausscpu-llvm.git 2fa6958fe09425038e3a874dcc79450a100d0a3b)"
	.section	".note.GNU-stack","",@progbits
	.addrsig

/* SPDX-License-Identifier: GPL-2.0 */
/*
 * User copies.  When both addresses share their offset within a doubleword
 * (and n >= 16): bytes up to an aligned destination, then four doublewords
 * (one 32-byte line) per pass, then single doublewords, then bytes.
 * Otherwise a byte at a time (64-bit accesses must be aligned).
 *
 * Each user access has an __ex_table entry.  A fault in a doubleword or a
 * group of four restarts that group a byte at a time (r0, r1 and r2 are
 * still the group's start: n is counted down only after a whole group, and
 * redoing a store already done is harmless); the byte loop's fixup returns
 * n, so the count of bytes not copied is exact.  It must be: a group can
 * straddle a page boundary, and returning "nothing copied" while bytes
 * before the boundary were accessible breaks raw_copy_{to,from}_user's
 * contract (include/linux/uaccess.h): generic_perform_write would retry
 * forever.
 */
#include <linux/linkage.h>

.macro EX insn, fixup
	.pushsection __ex_table, "a"
	.balign 4
	.long \insn, \fixup
	.popsection
.endm

	.text
/* unsigned long raw_copy_from_user(void *to = r0, const void __user *from = r1, n = r2) */
ENTRY(raw_copy_from_user)
	fbrv	r2, 16, 3, 8f		/* n < 16: bytes */
	xorr	r3, r0, r1
	andv	r3, r3, 7
	fbri	r3, 0, 8, 8f		/* different offsets: bytes */
1:	andv	r3, r0, 7		/* head: bytes until to is aligned */
	fbri	r3, 0, 0, 3f
2:	ldidx8	r3, r1, 0
	EX	2b, 9f
	stidx8	r3, r0, 0
	addi	r0, r0, 1
	addi	r1, r1, 1
	addi	r2, r2, -1
	jmp	1b
3:	fbrv	r2, 32, 3, 5f		/* lines */
40:	ldidx64	r3, r1, 0
41:	ldidx64	r12, r1, 8
42:	ldidx64	r13, r1, 16
43:	ldidx64	r14, r1, 24
	EX	40b, 8f
	EX	41b, 8f
	EX	42b, 8f
	EX	43b, 8f
	stidx64	r3, r0, 0
	stidx64	r12, r0, 8
	stidx64	r13, r0, 16
	stidx64	r14, r0, 24
	addi	r0, r0, 32
	addi	r1, r1, 32
	addi	r2, r2, -32
	jmp	3b
5:	fbrv	r2, 8, 3, 8f		/* doublewords */
6:	ldidx64	r3, r1, 0
	EX	6b, 8f
	stidx64	r3, r0, 0
	addi	r0, r0, 8
	addi	r1, r1, 8
	addi	r2, r2, -8
	jmp	5b
8:	fbri	r2, 0, 0, 9f		/* bytes */
7:	ldidx8	r3, r1, 0
	EX	7b, 9f
	stidx8	r3, r0, 0
	addi	r0, r0, 1
	addi	r1, r1, 1
	addi	r2, r2, -1
	jmp	8b
9:	copy	r12, r2
	ret
ENDPROC(raw_copy_from_user)

/* unsigned long raw_copy_to_user(void __user *to = r0, const void *from = r1, n = r2) */
ENTRY(raw_copy_to_user)
	fbrv	r2, 16, 3, 8f
	xorr	r3, r0, r1
	andv	r3, r3, 7
	fbri	r3, 0, 8, 8f
1:	andv	r3, r0, 7
	fbri	r3, 0, 0, 3f
	ldidx8	r3, r1, 0
2:	stidx8	r3, r0, 0
	EX	2b, 9f
	addi	r0, r0, 1
	addi	r1, r1, 1
	addi	r2, r2, -1
	jmp	1b
3:	fbrv	r2, 32, 3, 5f
	ldidx64	r3, r1, 0
	ldidx64	r12, r1, 8
	ldidx64	r13, r1, 16
	ldidx64	r14, r1, 24
40:	stidx64	r3, r0, 0
41:	stidx64	r12, r0, 8
42:	stidx64	r13, r0, 16
43:	stidx64	r14, r0, 24
	EX	40b, 8f
	EX	41b, 8f
	EX	42b, 8f
	EX	43b, 8f
	addi	r0, r0, 32
	addi	r1, r1, 32
	addi	r2, r2, -32
	jmp	3b
5:	fbrv	r2, 8, 3, 8f
	ldidx64	r3, r1, 0
6:	stidx64	r3, r0, 0
	EX	6b, 8f
	addi	r0, r0, 8
	addi	r1, r1, 8
	addi	r2, r2, -8
	jmp	5b
8:	fbri	r2, 0, 0, 9f
	ldidx8	r3, r1, 0
7:	stidx8	r3, r0, 0
	EX	7b, 9f
	addi	r0, r0, 1
	addi	r1, r1, 1
	addi	r2, r2, -1
	jmp	8b
9:	copy	r12, r2
	ret
ENDPROC(raw_copy_to_user)

/* unsigned long __clear_user(void __user *addr = r0, n = r1) */
ENTRY(__clear_user)
	setr	r3, 0
	fbrv	r1, 16, 3, 8f
1:	andv	r2, r0, 7
	fbri	r2, 0, 0, 3f
2:	stidx8	r3, r0, 0
	EX	2b, 9f
	addi	r0, r0, 1
	addi	r1, r1, -1
	jmp	1b
3:	fbrv	r1, 32, 3, 5f
40:	stidx64	r3, r0, 0
41:	stidx64	r3, r0, 8
42:	stidx64	r3, r0, 16
43:	stidx64	r3, r0, 24
	EX	40b, 8f
	EX	41b, 8f
	EX	42b, 8f
	EX	43b, 8f
	addi	r0, r0, 32
	addi	r1, r1, -32
	jmp	3b
5:	fbrv	r1, 8, 3, 8f
6:	stidx64	r3, r0, 0
	EX	6b, 8f
	addi	r0, r0, 8
	addi	r1, r1, -8
	jmp	5b
8:	fbri	r1, 0, 0, 9f
7:	stidx8	r3, r0, 0
	EX	7b, 9f
	addi	r0, r0, 1
	addi	r1, r1, -1
	jmp	8b
9:	copy	r12, r1
	ret
ENDPROC(__clear_user)

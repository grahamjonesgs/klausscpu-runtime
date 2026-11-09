/* SPDX-License-Identifier: GPL-2.0 */
/*
 * User copies, a byte at a time for now; each user access has an
 * __ex_table entry whose fixup returns the bytes left.
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
1:	fbri	r2, 0, 0, 9f
2:	ldidx8	r3, r1, 0
	EX	2b, 9f
	stidx8	r3, r0, 0
	addi	r0, r0, 1
	addi	r1, r1, 1
	addi	r2, r2, -1
	jmp	1b
9:	copy	r12, r2
	ret
ENDPROC(raw_copy_from_user)

/* unsigned long raw_copy_to_user(void __user *to = r0, const void *from = r1, n = r2) */
ENTRY(raw_copy_to_user)
1:	fbri	r2, 0, 0, 9f
	ldidx8	r3, r1, 0
2:	stidx8	r3, r0, 0
	EX	2b, 9f
	addi	r0, r0, 1
	addi	r1, r1, 1
	addi	r2, r2, -1
	jmp	1b
9:	copy	r12, r2
	ret
ENDPROC(raw_copy_to_user)

/* unsigned long __clear_user(void __user *addr = r0, n = r1) */
ENTRY(__clear_user)
	setr	r3, 0
1:	fbri	r1, 0, 0, 9f
2:	stidx8	r3, r0, 0
	EX	2b, 9f
	addi	r0, r0, 1
	addi	r1, r1, -1
	jmp	1b
9:	copy	r12, r1
	ret
ENDPROC(__clear_user)

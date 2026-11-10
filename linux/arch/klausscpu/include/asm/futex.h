/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Futex operations on user memory with LR/SC.  A fault on the lr32s or
 * sc32 (page faults are disabled here) goes through the exception table
 * and returns -EFAULT; the futex core then faults the page in and retries.
 */
#ifndef _ASM_KLAUSSCPU_FUTEX_H
#define _ASM_KLAUSSCPU_FUTEX_H

#include <linux/futex.h>
#include <linux/uaccess.h>
#include <linux/errno.h>

#define __futex_atomic_op(insn, ret, oldval, uaddr, oparg)		\
{									\
	long __new, __fail;						\
	asm volatile(							\
	"1:	lr32s %1, %4\n"						\
	"	" insn "\n"						\
	"2:	sc32 %3, %2, %4\n"					\
	"	fbri %3, 0, 8, 1b\n"					\
	"3:\n"								\
	".pushsection .fixup, \"ax\"\n"					\
	"4:	setr %0, %6\n"						\
	"	jmp 3b\n"						\
	".popsection\n"							\
	__ex_entry(1b, 4b)						\
	__ex_entry(2b, 4b)						\
	: "+r"(ret), "=&r"(oldval), "=&r"(__new), "=&r"(__fail)		\
	: "r"(uaddr), "r"((long)(oparg)), "i"(-EFAULT)			\
	: "memory");							\
}

static inline int arch_futex_atomic_op_inuser(int op, int oparg, int *oval, u32 __user *uaddr)
{
	int ret = 0;
	long oldval = 0;

	if (!access_ok(uaddr, sizeof(u32)))
		return -EFAULT;

	switch (op) {
	case FUTEX_OP_SET:
		__futex_atomic_op("copy %2, %5", ret, oldval, uaddr, oparg);
		break;
	case FUTEX_OP_ADD:
		__futex_atomic_op("addr %2, %1, %5", ret, oldval, uaddr, oparg);
		break;
	case FUTEX_OP_OR:
		__futex_atomic_op("orr %2, %1, %5", ret, oldval, uaddr, oparg);
		break;
	case FUTEX_OP_ANDN:
		__futex_atomic_op("andr %2, %1, %5", ret, oldval, uaddr, ~oparg);
		break;
	case FUTEX_OP_XOR:
		__futex_atomic_op("xorr %2, %1, %5", ret, oldval, uaddr, oparg);
		break;
	default:
		ret = -ENOSYS;
	}
	if (!ret)
		*oval = oldval;
	return ret;
}

static inline int futex_atomic_cmpxchg_inatomic(u32 *uval, u32 __user *uaddr,
						u32 oldval, u32 newval)
{
	int ret = 0;
	long val = 0, fail;

	if (!access_ok(uaddr, sizeof(u32)))
		return -EFAULT;

	asm volatile(
	"1:	lr32s %1, %3\n"
	"	fbr %1, %4, 8, 3f\n"		/* NE: no store */
	"2:	sc32 %2, %5, %3\n"
	"	fbri %2, 0, 8, 1b\n"
	"3:\n"
	".pushsection .fixup, \"ax\"\n"
	"4:	setr %0, %6\n"
	"	jmp 3b\n"
	".popsection\n"
	__ex_entry(1b, 4b)
	__ex_entry(2b, 4b)
	: "+r"(ret), "=&r"(val), "=&r"(fail)
	: "r"(uaddr), "r"((long)(int)oldval), "r"((long)newval), "i"(-EFAULT)
	: "memory");
	*uval = val;
	return ret;
}

#endif

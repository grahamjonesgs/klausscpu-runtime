/* SPDX-License-Identifier: GPL-2.0 */
/*
 * User memory is reached directly through useg (TLB R/W/X apply in
 * supervisor mode too, MMU_PLAN §11.3).  Every access that can fault has an
 * __ex_table entry; the copy routines are in arch/klausscpu/lib/uaccess.S.
 */
#ifndef _ASM_KLAUSSCPU_UACCESS_H
#define _ASM_KLAUSSCPU_UACCESS_H

#include <linux/types.h>
#include <asm/extable.h>
#include <asm/pgtable.h>

#define __ex_entry(insn, fixup)			\
	".pushsection __ex_table, \"a\"\n"	\
	"\t.balign 4\n"				\
	"\t.long " #insn ", " #fixup "\n"	\
	".popsection\n"

/* One user load or store of 1/2/4/8 bytes; *err = -EFAULT on a fault. */
#define __get_user_asm(insn, x, ptr, err)				\
	asm volatile(							\
	"1:\t" insn " %1, %2, 0\n"					\
	"2:\n"								\
	".pushsection .fixup, \"ax\"\n"					\
	"3:\tsetr %0, %3\n"						\
	"\tsetr %1, 0\n"						\
	"\tjmp 2b\n"							\
	".popsection\n"							\
	__ex_entry(1b, 3b)						\
	: "+r"(err), "=&r"(x)						\
	: "r"(ptr), "i"(-EFAULT))

#define __put_user_asm(insn, x, ptr, err)				\
	asm volatile(							\
	"1:\t" insn " %1, %2, 0\n"					\
	"2:\n"								\
	".pushsection .fixup, \"ax\"\n"					\
	"3:\tsetr %0, %3\n"						\
	"\tjmp 2b\n"							\
	".popsection\n"							\
	__ex_entry(1b, 3b)						\
	: "+r"(err)							\
	: "r"(x), "r"(ptr), "i"(-EFAULT))

#define __get_user_fn(sz, ptr, x)					\
({									\
	int __gu_err = 0;						\
	unsigned long __gu_val;						\
	switch (sz) {							\
	case 1: __get_user_asm("ldidx8", __gu_val, ptr, __gu_err); break; \
	case 2: __get_user_asm("ldidx16", __gu_val, ptr, __gu_err); break; \
	case 4: __get_user_asm("ldidx32", __gu_val, ptr, __gu_err); break; \
	case 8: __get_user_asm("ldidx64", __gu_val, ptr, __gu_err); break; \
	default: BUILD_BUG();						\
	}								\
	*(x) = (__typeof__(*(x)))__gu_val;				\
	__gu_err;							\
})

#define __put_user_fn(sz, ptr, x)					\
({									\
	int __pu_err = 0;						\
	unsigned long __pu_val = (unsigned long)*(x);			\
	switch (sz) {							\
	case 1: __put_user_asm("stidx8", __pu_val, ptr, __pu_err); break; \
	case 2: __put_user_asm("stidx16", __pu_val, ptr, __pu_err); break; \
	case 4: __put_user_asm("stidx32", __pu_val, ptr, __pu_err); break; \
	case 8: __put_user_asm("stidx64", __pu_val, ptr, __pu_err); break; \
	default: BUILD_BUG();						\
	}								\
	__pu_err;							\
})

extern unsigned long __must_check raw_copy_from_user(void *to, const void __user *from,
						     unsigned long n);
extern unsigned long __must_check raw_copy_to_user(void __user *to, const void *from,
						   unsigned long n);
extern unsigned long __must_check __clear_user(void __user *addr, unsigned long n);
#define __clear_user __clear_user
#define INLINE_COPY_FROM_USER
#define INLINE_COPY_TO_USER

#include <asm-generic/uaccess.h>

#endif

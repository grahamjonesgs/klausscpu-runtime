/* SPDX-License-Identifier: GPL-2.0 */
/*
 * xchg/cmpxchg with LR/SC.  4- and 8-byte values use lr32s/sc32 and
 * lr64/sc64 (32-bit values sit sign-extended in registers, as the ABI
 * keeps them); 1- and 2-byte ones loop on the aligned word that holds them.
 */
#ifndef _ASM_KLAUSSCPU_CMPXCHG_H
#define _ASM_KLAUSSCPU_CMPXCHG_H

#include <linux/types.h>
#include <linux/build_bug.h>

static inline unsigned long __kl_xchg_small(volatile void *ptr, unsigned long new, int size)
{
	unsigned long shift = ((unsigned long)ptr & 3) * 8;
	unsigned long mask = ((1UL << (size * 8)) - 1) << shift;
	volatile u32 *p = (volatile u32 *)((unsigned long)ptr & ~3UL);
	unsigned long old, tmp, fail;

	new = (new << shift) & mask;
	asm volatile(
	"1:	lr32 %0, %3\n"
	"	andr %1, %0, %4\n"
	"	orr %1, %1, %5\n"
	"	sc32 %2, %1, %3\n"
	"	fbri %2, 0, 8, 1b\n"
	: "=&r"(old), "=&r"(tmp), "=&r"(fail)
	: "r"(p), "r"(~mask), "r"(new)
	: "memory");
	return (old & mask) >> shift;
}

static inline unsigned long __kl_cmpxchg_small(volatile void *ptr, unsigned long exp,
					       unsigned long new, int size)
{
	unsigned long shift = ((unsigned long)ptr & 3) * 8;
	unsigned long mask = ((1UL << (size * 8)) - 1) << shift;
	volatile u32 *p = (volatile u32 *)((unsigned long)ptr & ~3UL);
	unsigned long old, tmp, fail;

	exp = (exp << shift) & mask;
	new = (new << shift) & mask;
	asm volatile(
	"1:	lr32 %0, %3\n"
	"	andr %1, %0, %4\n"
	"	fbr %1, %6, 8, 2f\n"		/* NE: no store */
	"	andr %1, %0, %5\n"
	"	orr %1, %1, %7\n"
	"	sc32 %2, %1, %3\n"
	"	fbri %2, 0, 8, 1b\n"
	"2:"
	: "=&r"(old), "=&r"(tmp), "=&r"(fail)
	: "r"(p), "r"(mask), "r"(~mask), "r"(exp), "r"(new)
	: "memory");
	return (old & mask) >> shift;
}

static __always_inline unsigned long __kl_xchg(volatile void *ptr, unsigned long new, int size)
{
	unsigned long old, fail;

	switch (size) {
	case 1:
	case 2:
		return __kl_xchg_small(ptr, new, size);
	case 4:
		asm volatile(
		"1:	lr32s %0, %2\n"
		"	sc32 %1, %3, %2\n"
		"	fbri %1, 0, 8, 1b\n"
		: "=&r"(old), "=&r"(fail) : "r"(ptr), "r"(new) : "memory");
		return old;
	case 8:
		asm volatile(
		"1:	lr64 %0, %2\n"
		"	sc64 %1, %3, %2\n"
		"	fbri %1, 0, 8, 1b\n"
		: "=&r"(old), "=&r"(fail) : "r"(ptr), "r"(new) : "memory");
		return old;
	}
	BUILD_BUG();
	return 0;
}

static __always_inline unsigned long __kl_cmpxchg(volatile void *ptr, unsigned long exp,
						  unsigned long new, int size)
{
	unsigned long old, fail;

	switch (size) {
	case 1:
	case 2:
		return __kl_cmpxchg_small(ptr, exp, new, size);
	case 4:
		exp = (long)(int)exp;
		asm volatile(
		"1:	lr32s %0, %2\n"
		"	fbr %0, %3, 8, 2f\n"
		"	sc32 %1, %4, %2\n"
		"	fbri %1, 0, 8, 1b\n"
		"2:"
		: "=&r"(old), "=&r"(fail) : "r"(ptr), "r"(exp), "r"(new) : "memory");
		return old;
	case 8:
		asm volatile(
		"1:	lr64 %0, %2\n"
		"	fbr %0, %3, 8, 2f\n"
		"	sc64 %1, %4, %2\n"
		"	fbri %1, 0, 8, 1b\n"
		"2:"
		: "=&r"(old), "=&r"(fail) : "r"(ptr), "r"(exp), "r"(new) : "memory");
		return old;
	}
	BUILD_BUG();
	return 0;
}

#define arch_xchg(ptr, x)						\
	((__typeof__(*(ptr)))__kl_xchg((ptr), (unsigned long)(x), sizeof(*(ptr))))

#define arch_cmpxchg(ptr, o, n)						\
	((__typeof__(*(ptr)))__kl_cmpxchg((ptr), (unsigned long)(o),	\
					  (unsigned long)(n), sizeof(*(ptr))))

#define arch_cmpxchg_local	arch_cmpxchg
#define arch_cmpxchg64(ptr, o, n) ({					\
	BUILD_BUG_ON(sizeof(*(ptr)) != 8);				\
	arch_cmpxchg((ptr), (o), (n));					\
})
#define arch_cmpxchg64_local	arch_cmpxchg64

#endif

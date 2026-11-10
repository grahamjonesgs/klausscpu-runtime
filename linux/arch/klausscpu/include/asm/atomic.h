/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Atomics with LR/SC (lr32s/sc32, lr64/sc64; SC writes 0 on success and
 * clears the reservation, as does any exception or interrupt entry, so a
 * loop retries).  One in-order core: no fences beyond the compiler
 * barrier, so every operation is fully ordered.
 */
#ifndef _ASM_KLAUSSCPU_ATOMIC_H
#define _ASM_KLAUSSCPU_ATOMIC_H

#include <linux/types.h>
#include <asm/cmpxchg.h>
#include <asm/barrier.h>

#define ATOMIC64_INIT(i)	{ (i) }

#define arch_atomic_read(v)		READ_ONCE((v)->counter)
#define arch_atomic_set(v, i)		WRITE_ONCE(((v)->counter), (i))
#define arch_atomic64_read(v)		READ_ONCE((v)->counter)
#define arch_atomic64_set(v, i)		WRITE_ONCE(((v)->counter), (i))

#define KL_ATOMIC_OP(pfx, T, lr, sc, op, insn, c_op)			\
static inline void arch_##pfx##_##op(T i, pfx##_t *v)			\
{									\
	long t, fail;							\
	asm volatile(							\
	"1:	" lr " %0, %2\n"					\
	"	" insn " %0, %0, %3\n"					\
	"	" sc " %1, %0, %2\n"					\
	"	fbri %1, 0, 8, 1b\n"					\
	: "=&r"(t), "=&r"(fail)						\
	: "r"(&v->counter), "r"((long)i)				\
	: "memory");							\
}									\
static inline T arch_##pfx##_fetch_##op(T i, pfx##_t *v)		\
{									\
	long old, new, fail;						\
	asm volatile(							\
	"1:	" lr " %0, %3\n"					\
	"	" insn " %1, %0, %4\n"					\
	"	" sc " %2, %1, %3\n"					\
	"	fbri %2, 0, 8, 1b\n"					\
	: "=&r"(old), "=&r"(new), "=&r"(fail)				\
	: "r"(&v->counter), "r"((long)i)				\
	: "memory");							\
	return old;							\
}

#define KL_ATOMIC_RETURN(pfx, T, op, c_op)				\
static inline T arch_##pfx##_##op##_return(T i, pfx##_t *v)		\
{									\
	return arch_##pfx##_fetch_##op(i, v) c_op i;			\
}

#define KL_ATOMIC_ALL(pfx, T, lr, sc)					\
	KL_ATOMIC_OP(pfx, T, lr, sc, add, "addr", +)			\
	KL_ATOMIC_OP(pfx, T, lr, sc, sub, "subr", -)			\
	KL_ATOMIC_OP(pfx, T, lr, sc, and, "andr", &)			\
	KL_ATOMIC_OP(pfx, T, lr, sc, or, "orr", |)			\
	KL_ATOMIC_OP(pfx, T, lr, sc, xor, "xorr", ^)			\
	KL_ATOMIC_RETURN(pfx, T, add, +)				\
	KL_ATOMIC_RETURN(pfx, T, sub, -)

KL_ATOMIC_ALL(atomic, int, "lr32s", "sc32")
KL_ATOMIC_ALL(atomic64, s64, "lr64", "sc64")

#undef KL_ATOMIC_ALL
#undef KL_ATOMIC_RETURN
#undef KL_ATOMIC_OP

#define arch_atomic_add_return		arch_atomic_add_return
#define arch_atomic_sub_return		arch_atomic_sub_return
#define arch_atomic_fetch_add		arch_atomic_fetch_add
#define arch_atomic_fetch_sub		arch_atomic_fetch_sub
#define arch_atomic_fetch_and		arch_atomic_fetch_and
#define arch_atomic_fetch_or		arch_atomic_fetch_or
#define arch_atomic_fetch_xor		arch_atomic_fetch_xor
#define arch_atomic64_add_return	arch_atomic64_add_return
#define arch_atomic64_sub_return	arch_atomic64_sub_return
#define arch_atomic64_fetch_add		arch_atomic64_fetch_add
#define arch_atomic64_fetch_sub		arch_atomic64_fetch_sub
#define arch_atomic64_fetch_and		arch_atomic64_fetch_and
#define arch_atomic64_fetch_or		arch_atomic64_fetch_or
#define arch_atomic64_fetch_xor		arch_atomic64_fetch_xor

#define arch_atomic_xchg(v, new)		arch_xchg(&((v)->counter), (new))
#define arch_atomic_cmpxchg(v, old, new)	arch_cmpxchg(&((v)->counter), (old), (new))
#define arch_atomic64_xchg(v, new)		arch_xchg(&((v)->counter), (new))
#define arch_atomic64_cmpxchg(v, old, new)	arch_cmpxchg(&((v)->counter), (old), (new))

#endif

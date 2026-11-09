/* SPDX-License-Identifier: GPL-2.0 */
/*
 * UP atomics: each operation runs with interrupts off (DI/EI), which is
 * atomic on one core.  LR/SC versions come later (LINUX_PLAN.md).
 */
#ifndef _ASM_KLAUSSCPU_ATOMIC_H
#define _ASM_KLAUSSCPU_ATOMIC_H

#include <linux/types.h>
#include <linux/irqflags.h>
#include <asm/cmpxchg.h>
#include <asm/barrier.h>

#define ATOMIC64_INIT(i)	{ (i) }

#define arch_atomic_read(v)		READ_ONCE((v)->counter)
#define arch_atomic_set(v, i)		WRITE_ONCE(((v)->counter), (i))
#define arch_atomic64_read(v)		READ_ONCE((v)->counter)
#define arch_atomic64_set(v, i)		WRITE_ONCE(((v)->counter), (i))

#define KL_ATOMIC_OP(pfx, T, op, c_op)					\
static inline void arch_##pfx##_##op(T i, pfx##_t *v)			\
{									\
	unsigned long flags;						\
	raw_local_irq_save(flags);					\
	v->counter = v->counter c_op i;					\
	raw_local_irq_restore(flags);					\
}									\
static inline T arch_##pfx##_##op##_return(T i, pfx##_t *v)		\
{									\
	unsigned long flags;						\
	T ret;								\
	raw_local_irq_save(flags);					\
	ret = (v->counter = v->counter c_op i);				\
	raw_local_irq_restore(flags);					\
	return ret;							\
}									\
static inline T arch_##pfx##_fetch_##op(T i, pfx##_t *v)		\
{									\
	unsigned long flags;						\
	T ret;								\
	raw_local_irq_save(flags);					\
	ret = v->counter;						\
	v->counter = ret c_op i;					\
	raw_local_irq_restore(flags);					\
	return ret;							\
}

#define KL_ATOMIC_BITOP(pfx, T, op, c_op)				\
static inline void arch_##pfx##_##op(T i, pfx##_t *v)			\
{									\
	unsigned long flags;						\
	raw_local_irq_save(flags);					\
	v->counter = v->counter c_op i;					\
	raw_local_irq_restore(flags);					\
}									\
static inline T arch_##pfx##_fetch_##op(T i, pfx##_t *v)		\
{									\
	unsigned long flags;						\
	T ret;								\
	raw_local_irq_save(flags);					\
	ret = v->counter;						\
	v->counter = ret c_op i;					\
	raw_local_irq_restore(flags);					\
	return ret;							\
}

#define KL_ATOMIC_ALL(pfx, T)						\
	KL_ATOMIC_OP(pfx, T, add, +)					\
	KL_ATOMIC_OP(pfx, T, sub, -)					\
	KL_ATOMIC_BITOP(pfx, T, and, &)					\
	KL_ATOMIC_BITOP(pfx, T, or, |)					\
	KL_ATOMIC_BITOP(pfx, T, xor, ^)

KL_ATOMIC_ALL(atomic, int)
KL_ATOMIC_ALL(atomic64, s64)

#undef KL_ATOMIC_ALL
#undef KL_ATOMIC_BITOP
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
#define arch_atomic64_cmpxchg(v, old, new)	arch_cmpxchg64(&((v)->counter), (old), (new))

#endif

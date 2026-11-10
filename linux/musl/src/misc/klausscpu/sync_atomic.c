/*
 * The __sync_* libcalls the KlaussCPU backend emits for atomic
 * read-modify-write operations ("forced atomics"), for user programs, with
 * LR/SC.  ABI as the backend calls them: arguments and results in 64-bit
 * registers; for 1/2/4-byte variants the upper bits of the arguments are
 * undefined and the result must be zero-extended.  The names clash with
 * clang's builtins, so each is bound with an asm label.  1- and 2-byte
 * variants loop on the aligned word that holds them.
 */
#include <stdint.h>

static inline uint64_t lr32(volatile void *p)
{
	uint64_t v;
	__asm__ __volatile__ ("lr32 %0, %1" : "=r"(v) : "r"(p) : "memory");
	return v;
}
static inline uint64_t sc32(volatile void *p, uint64_t v)
{
	uint64_t f;
	__asm__ __volatile__ ("sc32 %0, %1, %2" : "=r"(f) : "r"(v), "r"(p) : "memory");
	return f;
}
static inline uint64_t lr64(volatile void *p)
{
	uint64_t v;
	__asm__ __volatile__ ("lr64 %0, %1" : "=r"(v) : "r"(p) : "memory");
	return v;
}
static inline uint64_t sc64(volatile void *p, uint64_t v)
{
	uint64_t f;
	__asm__ __volatile__ ("sc64 %0, %1, %2" : "=r"(f) : "r"(v), "r"(p) : "memory");
	return f;
}

/* old = *p; *p = NEWEXPR(old) atomically; result = old (zero-extended). */
#define RMW(N, T, p, NEWEXPR) ({						\
	uint64_t _old;								\
	if (N == 8) {								\
		do _old = lr64(p);						\
		while (sc64(p, (uint64_t)(T)(NEWEXPR)));			\
	} else if (N == 4) {							\
		do _old = lr32(p);						\
		while (sc32(p, (uint64_t)(T)(NEWEXPR)));			\
	} else {								\
		volatile void *_w = (volatile void *)((uintptr_t)(p) & ~(uintptr_t)3); \
		unsigned _sh = ((uintptr_t)(p) & 3) * 8;			\
		uint64_t _m = (uint64_t)((T)~(T)0) << _sh, _wv, _nw;		\
		do {								\
			_wv = lr32(_w);						\
			_old = (_wv & _m) >> _sh;				\
			_nw = (_wv & ~_m) | (((uint64_t)(T)(NEWEXPR) << _sh) & _m); \
		} while (sc32(_w, _nw));					\
	}									\
	(uint64_t)(T)_old; })

#define FETCH_AND(OP, N, T, S, NEW)						\
uint64_t __kl_sync_fetch_and_##OP##_##N(volatile void *p, uint64_t v)		\
	__asm__("__sync_fetch_and_" #OP "_" #N);				\
uint64_t __kl_sync_fetch_and_##OP##_##N(volatile void *p, uint64_t v)		\
{										\
	T val = (T)v;								\
	return RMW(N, T, p, ({ T old = (T)_old; (void)old; NEW; }));		\
}

#define ALL(N, T, S)								\
	FETCH_AND(add,  N, T, S, old + val)					\
	FETCH_AND(sub,  N, T, S, old - val)					\
	FETCH_AND(and,  N, T, S, old & val)					\
	FETCH_AND(or,   N, T, S, old | val)					\
	FETCH_AND(xor,  N, T, S, old ^ val)					\
	FETCH_AND(nand, N, T, S, ~(old & val))					\
	FETCH_AND(max,  N, T, S, (S)old > (S)val ? old : val)			\
	FETCH_AND(min,  N, T, S, (S)old < (S)val ? old : val)			\
	FETCH_AND(umax, N, T, S, old > val ? old : val)				\
	FETCH_AND(umin, N, T, S, old < val ? old : val)				\
uint64_t __kl_sync_lock_test_and_set_##N(volatile void *p, uint64_t v)		\
	__asm__("__sync_lock_test_and_set_" #N);				\
uint64_t __kl_sync_lock_test_and_set_##N(volatile void *p, uint64_t v)		\
{										\
	return RMW(N, T, p, v);							\
}										\
uint64_t __kl_sync_val_compare_and_swap_##N(volatile void *p, uint64_t e,	\
					    uint64_t v)				\
	__asm__("__sync_val_compare_and_swap_" #N);				\
uint64_t __kl_sync_val_compare_and_swap_##N(volatile void *p, uint64_t e,	\
					    uint64_t v)				\
{										\
	/* A failed compare stores the old value back: still atomic. */	\
	return RMW(N, T, p, (T)_old == (T)e ? (T)v : (T)_old);			\
}

ALL(1, uint8_t,  int8_t)
ALL(2, uint16_t, int16_t)
ALL(4, uint32_t, int32_t)
ALL(8, uint64_t, int64_t)

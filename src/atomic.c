// atomic.c — the __sync_* libcalls behind C/C++ atomics on KlaussCPU.
//
// The ISA has no atomic read-modify-write, compare-and-swap or LL/SC.  The
// backend uses the "forced atomics" model (as RISC-V +forced-atomics does):
//  - naturally aligned atomic loads and stores of 1/2/4/8 bytes are plain
//    loads and stores (the in-order core makes every access visible in
//    program order, also to its own interrupt handlers);
//  - fences are compiler barriers;
//  - every RMW and cmpxchg of 1/2/4/8 bytes is a call to one of the
//    functions below (LLVM's __sync_* libcall set: RuntimeLibcalls.td);
//  - larger or misaligned atomics are generic __atomic_* libcalls, which
//    this runtime does not provide (the link fails).
//
// Each function is a critical section: save INT_MASK, write 0, do the plain
// operation, restore the saved mask.  A store of 0 to INT_MASK is a precise
// interrupt disable: an interrupt that was about to be taken is re-checked
// once the pipe has drained and abandoned (CPU_ARCHITECTURE.md §13.1).  An
// interrupt between the read and the write of INT_MASK is harmless, since
// IRET restores the mask its handler found.
//
// ATOMIC WITHIN ONE CORE ONLY.  This excludes the core's own interrupt
// handlers and (through them) RTOS preemption.  It does nothing against the
// second (AMP) core, whose view of DDR also bypasses core 1's write-back
// D-cache: talk to core 2 through the mailbox, with an explicit cache flush.
//
// The functions use volatile plain accesses only, so this file never turns
// into atomic instructions (or calls to itself).
//
// ABI, as the backend calls them (all arguments and results in 64-bit
// registers): for the 1/2/4-byte variants the upper bits of the value
// arguments are undefined (LLVM any-extends them), and the result must be
// zero-extended (getExtendForAtomicOps() is ZERO_EXTEND, so the caller
// assumes it).  Hence every function takes and returns uint64_t and
// truncates / zero-extends itself.  The C names would clash with clang's
// __sync builtins, so each symbol is bound with an asm label.
//
// Build: compiled into every program with the program's flags
// (-ffunction-sections + --gc-sections keep only the ones used).

#include <stdint.h>
#include "../mmio.h"

static inline uint32_t irq_save(void) {
    uint32_t mask = REG_INT_MASK;
    REG_INT_MASK = 0;
    __asm__ volatile("" ::: "memory");
    return mask;
}

static inline void irq_restore(uint32_t mask) {
    __asm__ volatile("" ::: "memory");
    REG_INT_MASK = mask;
}

// T: the unsigned type of the object; S: its signed twin (for min/max).
// `old` is the value in memory, `val` the operand, both of type T.
#define SYNC_FETCH_AND(OP, N, T, S, NEW)                                     \
    uint64_t sync_fetch_and_##OP##_##N(volatile void *ptr, uint64_t arg)     \
        __asm__("__sync_fetch_and_" #OP "_" #N);                             \
    uint64_t sync_fetch_and_##OP##_##N(volatile void *ptr, uint64_t arg) {   \
        volatile T *p = (volatile T *)ptr;                                   \
        T val = (T)arg;                                                      \
        uint32_t mask = irq_save();                                          \
        T old = *p;                                                          \
        *p = (T)(NEW);                                                       \
        irq_restore(mask);                                                   \
        return (uint64_t)old;                                                \
    }

#define SYNC_ALL(N, T, S)                                                    \
    SYNC_FETCH_AND(add,  N, T, S, old + val)                                 \
    SYNC_FETCH_AND(sub,  N, T, S, old - val)                                 \
    SYNC_FETCH_AND(and,  N, T, S, old & val)                                 \
    SYNC_FETCH_AND(or,   N, T, S, old | val)                                 \
    SYNC_FETCH_AND(xor,  N, T, S, old ^ val)                                 \
    SYNC_FETCH_AND(nand, N, T, S, ~(old & val))                              \
    SYNC_FETCH_AND(max,  N, T, S, (S)old > (S)val ? old : val)               \
    SYNC_FETCH_AND(min,  N, T, S, (S)old < (S)val ? old : val)               \
    SYNC_FETCH_AND(umax, N, T, S, old > val ? old : val)                     \
    SYNC_FETCH_AND(umin, N, T, S, old < val ? old : val)                     \
                                                                             \
    /* atomicrmw xchg */                                                     \
    uint64_t sync_lock_test_and_set_##N(volatile void *ptr, uint64_t arg)    \
        __asm__("__sync_lock_test_and_set_" #N);                             \
    uint64_t sync_lock_test_and_set_##N(volatile void *ptr, uint64_t arg) {  \
        volatile T *p = (volatile T *)ptr;                                   \
        uint32_t mask = irq_save();                                          \
        T old = *p;                                                          \
        *p = (T)arg;                                                         \
        irq_restore(mask);                                                   \
        return (uint64_t)old;                                                \
    }                                                                        \
                                                                             \
    /* cmpxchg: returns the old value; the caller compares it */             \
    uint64_t sync_val_compare_and_swap_##N(volatile void *ptr,               \
                                           uint64_t expected,                \
                                           uint64_t desired)                 \
        __asm__("__sync_val_compare_and_swap_" #N);                          \
    uint64_t sync_val_compare_and_swap_##N(volatile void *ptr,               \
                                           uint64_t expected,                \
                                           uint64_t desired) {               \
        volatile T *p = (volatile T *)ptr;                                   \
        uint32_t mask = irq_save();                                          \
        T old = *p;                                                          \
        if (old == (T)expected)                                              \
            *p = (T)desired;                                                 \
        irq_restore(mask);                                                   \
        return (uint64_t)old;                                                \
    }

SYNC_ALL(1, uint8_t,  int8_t)
SYNC_ALL(2, uint16_t, int16_t)
SYNC_ALL(4, uint32_t, int32_t)
SYNC_ALL(8, uint64_t, int64_t)

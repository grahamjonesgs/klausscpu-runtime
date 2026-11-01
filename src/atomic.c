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
// Each function is a critical section: DI (disable interrupts, returning the
// old enable bit), the plain operation, EI with that bit.  DI/EI are the
// core's interrupt-enable bit (ISA_ENCODING_V2_MAP.md §8.2), separate from
// INT_MASK, which stays what it is for: the per-source mask.  An IRQ is never
// taken between a retired DI and its EI (one that was about to be taken is
// re-checked once the pipe has drained and abandoned), and WAIT still wakes
// without entering a handler.  The old bit is what EI restores, so a section
// nests inside another one (or inside an interrupt handler, where IRET
// restores the enable bit it saved in the frame's bit 43) and leaves interrupts
// as it found them.  DI;EI is about 3 cycles; the INT_MASK save / zero /
// restore it replaces was about 16.5.
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

// The builtins are compiler barriers for memory (llvm.klausscpu.di / .ei read
// and write memory), so the accesses between them stay between them.
static inline uint32_t irq_save(void) {
    return __builtin_klausscpu_di();
}

static inline void irq_restore(uint32_t key) {
    __builtin_klausscpu_ei(key);
}

// T: the unsigned type of the object; S: its signed twin (for min/max).
// `old` is the value in memory, `val` the operand, both of type T.
#define SYNC_FETCH_AND(OP, N, T, S, NEW)                                     \
    uint64_t sync_fetch_and_##OP##_##N(volatile void *ptr, uint64_t arg)     \
        __asm__("__sync_fetch_and_" #OP "_" #N);                             \
    uint64_t sync_fetch_and_##OP##_##N(volatile void *ptr, uint64_t arg) {   \
        volatile T *p = (volatile T *)ptr;                                   \
        T val = (T)arg;                                                      \
        uint32_t key = irq_save();                                           \
        T old = *p;                                                          \
        *p = (T)(NEW);                                                       \
        irq_restore(key);                                                    \
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
        uint32_t key = irq_save();                                           \
        T old = *p;                                                          \
        *p = (T)arg;                                                         \
        irq_restore(key);                                                    \
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
        uint32_t key = irq_save();                                           \
        T old = *p;                                                          \
        if (old == (T)expected)                                              \
            *p = (T)desired;                                                 \
        irq_restore(key);                                                    \
        return (uint64_t)old;                                                \
    }

SYNC_ALL(1, uint8_t,  int8_t)
SYNC_ALL(2, uint16_t, int16_t)
SYNC_ALL(4, uint32_t, int32_t)
SYNC_ALL(8, uint64_t, int64_t)

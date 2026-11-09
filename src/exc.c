// exc.c — bare-metal exception and user-mode support (MMU stage 1, exc.h).
//
// exc_init() installs exc_vector (src/exc_vector.S) as EVEC on a core that
// has CPU_FEATURES bit 0.  Every exception then lands in exc_dispatch() with
// the registers saved; the program's handler decides what to do, and the
// default prints the state and halts with R12 = 0x100 | cause.

#include <stdio.h>

#include "exc.h"
#include "mmio.h"

// picolibc's <setjmp.h> has no jmp_buf for KlaussCPU: src/setjmp.S takes a
// buffer of 7 slots of 8 bytes (see programs/test_setjmp.c).
typedef long klauss_jmp_buf[7];
__attribute__((returns_twice)) int setjmp(long *env);
__attribute__((noreturn)) void longjmp(long *env, int val);

uint32_t __klauss_cpu_features;

static exc_handler_t g_handler;

// exc_run_user state.
static klauss_jmp_buf g_user_jb;
static volatile int g_user_active;
static volatile uint64_t g_user_result;

extern void exc_vector(void);
extern void exc_user_return(void);
extern void exc_enter_user(uint64_t frame, uint64_t arg, uint64_t usp)
    __attribute__((noreturn));

uint32_t exc_init(exc_handler_t h) {
    uint32_t f = *(volatile uint32_t *)CPU_FEATURES_ADDR;
    __klauss_cpu_features = f;
    if (!(f & CPU_FEAT_EXC))
        return 0;
    g_handler = h;
    exc_mtsr(SR_EVEC, (uintptr_t)exc_vector);
    return f;
}

void exc_disable(void) {
    if (__klauss_cpu_features & CPU_FEAT_EXC)
        exc_mtsr(SR_EVEC, 0);
}

static const char *cause_name(uint32_t c) {
    switch (c) {
    case EXC_ILLEGAL:          return "ILLEGAL";
    case EXC_PRIVILEGED:       return "PRIVILEGED";
    case EXC_TRAP:             return "TRAP";
    case EXC_SYSCALL:          return "SYSCALL";
    case EXC_MISALIGNED_LOAD:  return "MISALIGNED_LOAD";
    case EXC_MISALIGNED_STORE: return "MISALIGNED_STORE";
    case EXC_ACCESS_LOAD:      return "ACCESS_LOAD";
    case EXC_ACCESS_STORE:     return "ACCESS_STORE";
    case EXC_ACCESS_FETCH:     return "ACCESS_FETCH";
    case EXC_MISALIGNED_FETCH: return "MISALIGNED_FETCH";
    default:                   return "?";
    }
}

__attribute__((noreturn)) static void exc_report(struct exc_regs *r, uint32_t c,
                                                 uint32_t eaddr) {
    int user = (r->frame & FRAME_PM) != 0;
    printf("\n*** exception %u %s in %s mode: PC=%08lx EADDR=%08lx\n", c,
           cause_name(c), user ? "user" : "supervisor",
           (unsigned long)(r->frame & FRAME_PC_MASK), (unsigned long)eaddr);
    printf("    frame=%016llx", (unsigned long long)r->frame);
    if (user)
        printf(" USP=%08lx", (unsigned long)exc_mfsr(SR_USP));
    printf("\n");
    for (int i = 0; i < 16; i++)
        printf("    r%-2d=%016llx%s", i, (unsigned long long)r->r[i],
               (i & 1) ? "\n" : "");
    register long rc __asm__("r12") = 0x100 | c;
    __asm__ volatile("halt" : : "r"(rc));
    __builtin_unreachable();
}

void exc_dispatch(uint64_t cause, uint64_t eaddr, struct exc_regs *r) {
    uint32_t c = (uint32_t)cause;
    if (c == EXC_SYSCALL && r->r[0] == SYS_EXIT_USER && g_user_active &&
        (r->frame & FRAME_PM)) {
        g_user_result = r->r[1];
        g_user_active = 0;
        longjmp(g_user_jb, 1);   // drops this frame and the exception frame
    }
    if (g_handler && g_handler(r, c, (uint32_t)eaddr))
        return;
    exc_report(r, c, (uint32_t)eaddr);
}

uint64_t exc_run_user(uint64_t (*fn)(uint64_t), uint64_t arg, void *ustack_top) {
    uint32_t key = __builtin_klausscpu_di();
    if (setjmp(g_user_jb) == 0) {
        uint64_t *sp = (uint64_t *)((uintptr_t)ustack_top & ~(uintptr_t)7);
        *--sp = (uintptr_t)exc_user_return;          // fn's RET lands here
        uint64_t mask = REG_INT_MASK & 0xFu;
        uint64_t frame = (uint32_t)(uintptr_t)fn | (mask << FRAME_INT_MASK_SHIFT) |
                         FRAME_PM;                   // DI = 0: enabled in user mode
        g_user_active = 1;
        exc_enter_user(frame, arg, (uintptr_t)sp);
    }
    // Back from the exit syscall: supervisor mode, DI set by the entry.
    __builtin_klausscpu_ei(key);
    return g_user_result;
}

// arch/sys_arch.h — lwIP OS abstraction stub for NO_SYS=1.
//
// With NO_SYS=1 lwIP needs no threads, semaphores, or mailboxes.
// The only OS primitive required is sys_now() (provided in sys_arch.c).
// Critical-section protection disables interrupts (DI / EI, mmio.h).

#ifndef LWIP_ARCH_SYS_ARCH_H
#define LWIP_ARCH_SYS_ARCH_H

#include <stdint.h>
#include "../../mmio.h"   // klauss_irq_disable, klauss_irq_restore

// SYS_LIGHTWEIGHT_PROT=1: lwIP calls sys_arch_protect/unprotect around
// shared data structures.  On a single-core CPU, disabling interrupts is
// sufficient mutual exclusion; the protect value is the old enable bit, so
// nested sections leave interrupts as they found them.
typedef unsigned int sys_prot_t;

static inline sys_prot_t sys_arch_protect(void) {
    return klauss_irq_disable();
}
static inline void sys_arch_unprotect(sys_prot_t pval) {
    klauss_irq_restore(pval);
}

#endif // LWIP_ARCH_SYS_ARCH_H

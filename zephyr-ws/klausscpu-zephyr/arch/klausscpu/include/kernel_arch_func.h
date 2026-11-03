#ifndef ZEPHYR_ARCH_KLAUSSCPU_INCLUDE_KERNEL_ARCH_FUNC_H_
#define ZEPHYR_ARCH_KLAUSSCPU_INCLUDE_KERNEL_ARCH_FUNC_H_

#ifndef _ASMLANGUAGE

#include <zephyr/sys/util.h>
#include "kernel_arch_data.h"

/* Set in ISR entry / cleared on ISR exit — no hardware "in-ISR" register. */
extern volatile uint32_t _klausscpu_in_isr;

static ALWAYS_INLINE bool arch_is_in_isr(void)
{
    return _klausscpu_in_isr != 0U;
}

static ALWAYS_INLINE void arch_kernel_init(void)
{
    /* Nothing: timer and UART initialised via SYS_INIT drivers. */
}

void z_klausscpu_fatal_error(unsigned int reason, const struct arch_esf *esf);

/* Context switch, in swap.S (also entered from the ISR exit path). */
void z_klausscpu_switch(void *switch_to, void **switched_from);

static ALWAYS_INLINE void arch_switch(void *switch_to, void **switched_from)
{
    z_klausscpu_switch(switch_to, switched_from);
}

#endif /* _ASMLANGUAGE */
#endif /* ZEPHYR_ARCH_KLAUSSCPU_INCLUDE_KERNEL_ARCH_FUNC_H_ */

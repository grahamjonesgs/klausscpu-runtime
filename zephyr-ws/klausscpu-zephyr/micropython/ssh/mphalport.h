/* mphalport.h — MicroPython HAL for the `python` command in ssh_shell.
 * Timing as ports/zephyr/mphalport.h; stdio and waits in mphalport.c. */
#ifndef KLAUSSCPU_MP_HALPORT_H
#define KLAUSSCPU_MP_HALPORT_H

#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include "shared/runtime/interrupt_char.h"

#define MICROPY_BEGIN_ATOMIC_SECTION irq_lock
#define MICROPY_END_ATOMIC_SECTION irq_unlock

/* Used by mp_event_wait_ms()/mp_event_wait_indefinite() (py/runtime.h). */
#define MICROPY_INTERNAL_WFE(TIMEOUT_MS) \
	mp_hal_wait_event(true, NULL, (TIMEOUT_MS) < 0 ? (uint32_t)-1 : (uint32_t)(TIMEOUT_MS))

/* As ports/zephyr: wait up to timeout_ms ((uint32_t)-1: forever), handling
 * pending exceptions (Ctrl-C) as it goes; return early on an event when
 * exit_on_event, or when sem becomes available. */
void mp_hal_wait_event(bool exit_on_event, struct k_sem *sem, uint32_t timeout_ms);

static inline mp_uint_t mp_hal_ticks_us(void)
{
	return k_cyc_to_us_floor64(k_cycle_get_64());
}

static inline mp_uint_t mp_hal_ticks_ms(void)
{
	return k_uptime_get();
}

static inline mp_uint_t mp_hal_ticks_cpu(void)
{
	return k_cycle_get_32();
}

static inline void mp_hal_delay_us(mp_uint_t us)
{
	k_busy_wait(us);
}

static inline void mp_hal_delay_ms(mp_uint_t ms)
{
	mp_hal_wait_event(false, NULL, ms);
}

static inline uint64_t mp_hal_time_ns(void)
{
	return k_ticks_to_ns_near64(k_uptime_ticks());
}

#define mp_hal_delay_us_fast(us) (mp_hal_delay_us(us))

#endif /* KLAUSSCPU_MP_HALPORT_H */

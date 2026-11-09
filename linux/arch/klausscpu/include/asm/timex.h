/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_KLAUSSCPU_TIMEX_H
#define _ASM_KLAUSSCPU_TIMEX_H

#include <asm/klausscpu.h>

typedef unsigned long cycles_t;

static inline cycles_t get_cycles(void)
{
	return mfsr(SR_CYCLE);
}
#define get_cycles get_cycles

#define CLOCK_TICK_RATE	CONFIG_KLAUSSCPU_CPU_HZ

#include <asm-generic/timex.h>

#endif

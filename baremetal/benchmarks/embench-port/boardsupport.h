/*
 * boardsupport.h — Embench-IoT board support for the KlaussCPU bare-metal
 * runtime.  Included by Embench's support/support.h (HAVE_BOARDSUPPORT_H).
 *
 * CPU_MHZ (the Embench iteration multiplier, not the real clock) and
 * WARMUP_HEAT come from the Makefile (-DCPU_MHZ=$(EMBENCH_CPU_MHZ)).  The
 * board's real core clock is 100 MHz; cycle counts are reported directly.
 */
#ifndef BOARDSUPPORT_H
#define BOARDSUPPORT_H

#ifndef CPU_MHZ
#define CPU_MHZ 1
#endif

#endif /* BOARDSUPPORT_H */

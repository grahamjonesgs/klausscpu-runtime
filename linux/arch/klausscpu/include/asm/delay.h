/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_KLAUSSCPU_DELAY_H
#define _ASM_KLAUSSCPU_DELAY_H

extern void __delay(unsigned long cycles);
extern void __udelay(unsigned long usecs);
extern void __ndelay(unsigned long nsecs);
extern void __const_udelay(unsigned long xloops);

#define udelay(n)	__udelay(n)
#define ndelay(n)	__ndelay(n)

#endif

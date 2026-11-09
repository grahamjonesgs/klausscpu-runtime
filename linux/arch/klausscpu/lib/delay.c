// SPDX-License-Identifier: GPL-2.0
#include <linux/delay.h>
#include <linux/export.h>
#include <asm/timex.h>

void __delay(unsigned long cycles)
{
	cycles_t start = get_cycles();

	while (get_cycles() - start < cycles)
		cpu_relax();
}
EXPORT_SYMBOL(__delay);

void __udelay(unsigned long usecs)
{
	__delay(usecs * (CONFIG_KLAUSSCPU_CPU_HZ / 1000000));
}
EXPORT_SYMBOL(__udelay);

void __ndelay(unsigned long nsecs)
{
	__delay(DIV_ROUND_UP(nsecs * (CONFIG_KLAUSSCPU_CPU_HZ / 1000000), 1000));
}
EXPORT_SYMBOL(__ndelay);

void __const_udelay(unsigned long xloops)
{
	__delay(xloops);
}
EXPORT_SYMBOL(__const_udelay);

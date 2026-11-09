// SPDX-License-Identifier: GPL-2.0
/* Boot console on the polled UART (MMIO, identity-mapped, no TLB needed). */
#include <linux/console.h>
#include <linux/init.h>
#include <linux/io.h>
#include <asm/klausscpu.h>

static void kl_uart_putc(char c)
{
	void __iomem *base = (void __iomem *)KL_UART_BASE;

	while (readl(base + KL_UART_STATUS) & KL_UART_TX_BUSY)
		cpu_relax();
	writel(c, base + KL_UART_TX);
}

static void kl_early_write(struct console *con, const char *s, unsigned int n)
{
	while (n--) {
		if (*s == '\n')
			kl_uart_putc('\r');
		kl_uart_putc(*s++);
	}
}

static struct console kl_early_console = {
	.name	= "klearly",
	.write	= kl_early_write,
	.flags	= CON_PRINTBUFFER | CON_BOOT,
	.index	= -1,
};

void __init klausscpu_early_console_init(void)
{
	if (IS_ENABLED(CONFIG_KLAUSSCPU_EARLY_UART))
		register_console(&kl_early_console);
}

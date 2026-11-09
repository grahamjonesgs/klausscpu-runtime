// SPDX-License-Identifier: GPL-2.0
/*
 * The polled UART as an HVC console (/dev/hvc0, console=hvc0).  The UART
 * has no RX interrupt, so HVC's polling thread (khvcd) reads it.
 */
#include <linux/console.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/err.h>
#include <asm/klausscpu.h>
#include "hvc_console.h"	/* drivers/tty/hvc (CFLAGS in the Makefile) */

static void __iomem *uart(unsigned int off)
{
	return (void __iomem *)(KL_UART_BASE + off);
}

static ssize_t kl_hvc_put(uint32_t vt, const u8 *buf, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++) {
		while (readl(uart(KL_UART_STATUS)) & KL_UART_TX_BUSY)
			cpu_relax();
		writel(buf[i], uart(KL_UART_TX));
	}
	return count;
}

static ssize_t kl_hvc_get(uint32_t vt, u8 *buf, size_t count)
{
	size_t i = 0;

	while (i < count && !(readl(uart(KL_UART_STATUS)) & KL_UART_RX_EMPTY))
		buf[i++] = readl(uart(KL_UART_RX));
	return i;
}

static const struct hv_ops kl_hvc_ops = {
	.get_chars = kl_hvc_get,
	.put_chars = kl_hvc_put,
};

static int __init kl_hvc_console_init(void)
{
	hvc_instantiate(0, 0, &kl_hvc_ops);
	return 0;
}
console_initcall(kl_hvc_console_init);

static int __init kl_hvc_init(void)
{
	return PTR_ERR_OR_ZERO(hvc_alloc(0, 0, &kl_hvc_ops, 256));
}
device_initcall(kl_hvc_init);

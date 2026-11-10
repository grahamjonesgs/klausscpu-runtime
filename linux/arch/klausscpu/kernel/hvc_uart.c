// SPDX-License-Identifier: GPL-2.0
/*
 * The polled UART as an HVC console (/dev/hvc0, console=hvc0).
 *
 * The UART has no RX interrupt and only a 16-byte receive FIFO.  HVC's
 * own polling (khvcd) backs off to once every 2 s when the console is
 * idle, so typed or pasted input overflowed the FIFO and was lost (a lost
 * newline looked like a hung shell).  Instead the timer interrupt drains
 * the FIFO every tick (kl_uart_rx_poll) into a ring, and kicks khvcd when
 * it has added anything; get_chars reads the ring.
 */
#include <linux/console.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/err.h>
#include <linux/irqflags.h>
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

/* Receive ring: written by kl_uart_rx_fill(), read by kl_hvc_get(), both
 * with interrupts off (one CPU), so no further locking is needed. */
#define KL_RX_RING	4096
static u8 kl_rx_ring[KL_RX_RING];
static unsigned int kl_rx_head, kl_rx_tail;	/* free-running */
static bool kl_hvc_ready;			/* hvc_alloc done: khvcd exists */

/* Move whatever the FIFO holds into the ring; returns how many bytes. */
static unsigned int kl_uart_rx_fill(void)
{
	unsigned int n = 0;

	while (!(readl(uart(KL_UART_STATUS)) & KL_UART_RX_EMPTY)) {
		u8 c = readl(uart(KL_UART_RX));

		if (kl_rx_head - kl_rx_tail < KL_RX_RING)
			kl_rx_ring[kl_rx_head++ % KL_RX_RING] = c;
		n++;
	}
	return n;
}

/* From the timer interrupt, every tick. */
void kl_uart_rx_poll(void)
{
	if (kl_uart_rx_fill() && kl_hvc_ready)
		hvc_kick();
}

static ssize_t kl_hvc_get(uint32_t vt, u8 *buf, size_t count)
{
	unsigned long flags;
	size_t i = 0;

	local_irq_save(flags);
	kl_uart_rx_fill();
	while (i < count && kl_rx_tail != kl_rx_head)
		buf[i++] = kl_rx_ring[kl_rx_tail++ % KL_RX_RING];
	local_irq_restore(flags);
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
	struct hvc_struct *hp = hvc_alloc(0, 0, &kl_hvc_ops, 256);

	if (IS_ERR(hp))
		return PTR_ERR(hp);
	kl_hvc_ready = true;
	return 0;
}
device_initcall(kl_hvc_init);

/*
 * uart_klausscpu.c — Zephyr UART driver for KlaussCPU.
 *
 * The UART is a polled, memory-mapped peripheral at 0xF001_0000:
 *   +0x00 TX     write  — low byte transmitted; transmitter busy until sent
 *   +0x08 RX     read   — pops one byte from the RX FIFO
 *   +0x10 STATUS read   — bit0 TX busy, bit1 RX empty, bit2 RX full
 *
 * poll_out waits for TX-idle then writes the byte; poll_in returns -1 when the
 * RX FIFO is empty.  (Previously used the TXCHARMEMR/RXRNB CPU instructions,
 * removed now that the UART lives on MMIO.)
 */
#define DT_DRV_COMPAT klausscpu_uart

#include <zephyr/kernel.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/device.h>

/* ── MMIO register definitions (mirrors runtime/mmio.h) ──────────────────── */

#define UART_BASE             0xF0010000u
#define REG(a)                (*(volatile uint32_t *)(unsigned long)(a))

#define REG_UART_TX           REG(UART_BASE + 0x0000u)
#define REG_UART_RX           REG(UART_BASE + 0x0008u)
#define REG_UART_STATUS       REG(UART_BASE + 0x0010u)

#define UART_STATUS_TX_BUSY   (1u << 0)
#define UART_STATUS_RX_EMPTY  (1u << 1)
#define UART_STATUS_RX_FULL   (1u << 2)

/* ── poll_in: pop RX FIFO, -1 when empty ─────────────────────────────────── */

static int uart_klausscpu_poll_in(const struct device *dev, unsigned char *c)
{
    ARG_UNUSED(dev);

    if (REG_UART_STATUS & UART_STATUS_RX_EMPTY) {
        return -1;
    }
    *c = (unsigned char)REG_UART_RX;
    return 0;
}

static void uart_klausscpu_poll_out(const struct device *dev, unsigned char c)
{
    ARG_UNUSED(dev);
    if (c == '\n') {
        while (REG_UART_STATUS & UART_STATUS_TX_BUSY) {
        }
        REG_UART_TX = '\r';
    }
    while (REG_UART_STATUS & UART_STATUS_TX_BUSY) {
    }
    REG_UART_TX = c;
}

static int uart_klausscpu_init(const struct device *dev)
{
    ARG_UNUSED(dev);
    return 0;
}

static const struct uart_driver_api uart_klausscpu_api = {
    .poll_in  = uart_klausscpu_poll_in,
    .poll_out = uart_klausscpu_poll_out,
};

DEVICE_DT_INST_DEFINE(0,
    uart_klausscpu_init,
    NULL,
    NULL,
    NULL,
    PRE_KERNEL_1,
    55,
    &uart_klausscpu_api);

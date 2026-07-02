// uart_stubs.c — UART I/O implementation using the KlaussCPU MMIO UART.
//
// The UART is a polled, memory-mapped peripheral at 0xF001_0000 (see mmio.h):
//   +0x00 TX     write  — low byte transmitted; transmitter busy until sent
//   +0x08 RX     read   — pops one byte from the RX FIFO
//   +0x10 STATUS read   — bit0 TX busy, bit1 RX empty, bit2 RX full
//
// (Previously this used the __builtin_klausscpu_{txr,txcharmemr,rxrb,rxrnb}
//  CPU instructions, which have been removed now that the UART lives on MMIO.)
//
// Compile with:
//   clang -target klausscpu-unknown-elf -O1 -nostdlib -ffreestanding \
//         -I<runtime-root> -c uart_stubs.c

#include <stdint.h>
#include "mmio.h"

/* Defined in syscalls.c — set by crt0_loadable before main() to redirect
 * console I/O to an active SSH/telnet session when running under the loader. */
extern void (*g_console_mirror_fn)(char c);
extern int  (*g_console_input_fn)(void);

// ---------------------------------------------------------------------------
// Transmit: send a 64-bit value as 16 hex digits over the UART (raw, no mirror).
// Replaces the old TXR instruction.
// ---------------------------------------------------------------------------
void uart_tx_hex(uint64_t val) {
    for (int shift = 60; shift >= 0; shift -= 4) {
        unsigned nyb = (unsigned)((val >> shift) & 0xFu);
        uart_tx_byte((uint8_t)(nyb < 10 ? '0' + nyb : 'A' + (nyb - 10)));
    }
}

// ---------------------------------------------------------------------------
// Transmit: send a single character (byte) over UART.
// '\n' is expanded to CR+LF so all output looks correct on a serial terminal.
// ---------------------------------------------------------------------------
void uart_putc(char c) {
    /* When a remote loader (SSH/telnet) is driving this program, redirect
     * output to that session only — not the physical UART.  The mirror
     * function does its own \n→\r\n expansion, so pass c unchanged. */
    if (g_console_mirror_fn) {
        g_console_mirror_fn(c);
        return;
    }
    if (c == '\n') uart_tx_byte((uint8_t)'\r');
    uart_tx_byte((uint8_t)c);
}

// ---------------------------------------------------------------------------
// Transmit: send null-terminated string over UART (with CR+LF conversion).
// ---------------------------------------------------------------------------
void uart_puts(const char *s) {
    while (*s) uart_putc(*s++);
}

// ---------------------------------------------------------------------------
// Transmit: CR+LF.
// ---------------------------------------------------------------------------
void uart_newline(void) {
    uart_putc('\n');   /* CR+LF to UART + mirror if active */
}

// ---------------------------------------------------------------------------
// Receive: blocking — wait for a UART byte, return it.
// ---------------------------------------------------------------------------
uint64_t uart_getc_blocking(void) {
    /* When a remote loader (SSH/telnet) is driving this program, read input
     * from that session instead of the physical UART RX FIFO. */
    if (g_console_input_fn)
        return (uint64_t)(unsigned char)g_console_input_fn();
    return (uint64_t)uart_rx_byte();
}

// ---------------------------------------------------------------------------
// Receive: non-blocking.  Returns the byte (0..255) if one was available, or
// (uint64_t)-1 when the RX FIFO is empty.
// ---------------------------------------------------------------------------
uint64_t uart_getc_nonblocking(void) {
    uint8_t c;
    if (uart_rx_try(&c)) return (uint64_t)c;
    return (uint64_t)-1;
}

// ---------------------------------------------------------------------------
// Convenience: puts() + newline.
// ---------------------------------------------------------------------------
void uart_println(const char *s) {
    uart_puts(s);
    uart_putc('\n');
}

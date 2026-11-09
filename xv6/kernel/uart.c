//
// klausscpu: driver for the KlaussCPU UART (runtime mmio.h).
//
// The UART is polled: TX_BUSY says the transmitter is still sending (a
// byte written then is lost), and there is no interrupt.  Output is
// synchronous; input is polled by clockintr() every tick.
//
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "klauss.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "defs.h"

static struct spinlock tx_lock;

extern volatile int panicking; // from printk.c
extern volatile int panicked;  // from printk.c

void
uartinit(void)
{
  initlock(&tx_lock, "uart");
}

static void
uart_tx(int c)
{
  while (UART_STATUS & UART_STATUS_TX_BUSY)
    ;
  UART_TX = c;
}

// transmit buf[] to the uart (from write() system calls).
void
uartwrite(char buf[], int n)
{
  acquire(&tx_lock);
  for (int i = 0; i < n; i++)
    uart_tx(buf[i]);
  release(&tx_lock);
}

// write a byte to the uart, for use by kernel printk() and to echo
// characters.
void
uartputc_sync(int c)
{
  if (panicking == 0)
    push_off();
  if (panicked) {
    for (;;)
      ;
  }
  uart_tx(c);
  if (panicking == 0)
    pop_off();
}

// try to read one input character from the UART.
// return -1 if none is waiting.
static int
uartgetc(void)
{
  if (UART_STATUS & UART_STATUS_RX_EMPTY)
    return -1;
  return UART_RX & 0xFF;
}

// process incoming characters, if any.  called from clockintr().
void
uartintr(void)
{
  while (1) {
    int c = uartgetc();
    if (c == -1)
      break;
    consoleintr(c);
  }
}

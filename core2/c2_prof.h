// c2_prof.h — exclusive-time profiler for AMP core 2.
//
// Core 2 can only read core 1's millisecond clock (mirror at 0xF00F_0040),
// so time is charged per *bucket*: every PROF_PUSH/PROF_POP charges the time
// since the last transition to the bucket on top of a small stack.  Buckets
// are exclusive (nested work is charged to the inner bucket only) and always
// sum to wall time.  A 1 ms clock still gives unbiased per-bucket totals
// because thousands of transitions per second land at random tick phases.
//
// Included from core2/lwipopts.h, so the shared lwip_port/ethernetif.c sees
// PROF_PUSH/PROF_POP in the core-2 build only (other builds get no-ops).
// Deliberately does not include mmio.h (its htons/htonl clash with lwIP's).
#ifndef C2_PROF_H
#define C2_PROF_H

#include <stdint.h>

#define C2_PROF_CLOCK_MS (*(volatile uint64_t *)0xF00F0040u)

enum {
    PROF_OTHER,      // main loop, timers, idle polling
    PROF_FETCH,      // framebuffer reads from DDR
    PROF_ENCODE,     // stage fill: copy / pixel convert / hextile
    PROF_TCP_WRITE,  // tcp_write (lwIP copies into its own buffers)
    PROF_TCP_OUT,    // tcp_output called by the VNC pump
    PROF_CHKSUM,     // Internet checksum (LWIP_CHKSUM)
    PROF_TX_WAIT,    // spinning on TX_READY (MAC FIFO full)
    PROF_TX_COPY,    // pbuf -> TX slot SRAM
    PROF_RX_COPY,    // RX slot SRAM -> pbuf
    PROF_RX_STACK,   // lwIP input processing (ACKs, and the output they trigger)
    PROF_N
};

extern uint32_t c2_prof_ms[PROF_N];
extern uint8_t  c2_prof_stack[16];
extern int      c2_prof_sp;
extern uint64_t c2_prof_t;

static inline void c2_prof_charge(void)
{
    uint64_t now = C2_PROF_CLOCK_MS;
    c2_prof_ms[c2_prof_stack[c2_prof_sp]] += (uint32_t)(now - c2_prof_t);
    c2_prof_t = now;
}

static inline void c2_prof_push(int bucket)
{
    c2_prof_charge();
    if (c2_prof_sp < 15) c2_prof_stack[++c2_prof_sp] = (uint8_t)bucket;
}

static inline void c2_prof_pop(void)
{
    c2_prof_charge();
    if (c2_prof_sp > 0) c2_prof_sp--;
}

#define PROF_PUSH(b) c2_prof_push(b)
#define PROF_POP()   c2_prof_pop()

#endif // C2_PROF_H

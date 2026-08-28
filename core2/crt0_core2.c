/*
 * crt0_core2.c — C runtime startup for AMP core 2.
 *
 * Differences from src/crt0.c (core 1):
 *   - SP is set by hardware to the top of core 2's local BRAM (0x0001_0000,
 *     pipeline_core SP_RESET) — nothing to do here.
 *   - .data lives in local BRAM but its initial image is in the DDR text
 *     window (core2.ld: VMA in LRAM, LMA in TEXT) — copy it.
 *   - No LED checkpoints (LED writes are void on core 2) and no UART settle
 *     delay (the console is the log FIFO, always ready).
 */
extern int  main(int argc, char **argv);
extern void __stdio_init(void);
#include "../amp/amp_proto.h"

extern char __data_start[], __data_end[], __data_load_start[];
extern char __bss_start[], __bss_end[], _end[];

__attribute__((noreturn, used))
void _start(void)
{
    const char *src = __data_load_start;
    for (char *p = __data_start; p != __data_end; ++p)
        *p = *src++;

    for (char *p = __bss_start; p != __bss_end; ++p)
        *p = 0;

    /* Heap header slot at (local) address 0, as on core 1. */
    *(volatile unsigned long long *)0 = (unsigned long long)(char *)_end;

    /* Crash breadcrumb: every pass through _start bumps the descriptor's
     * boot counter (consumer cache line) — a wild-jump restart is visible
     * from core 1 as pad1 > 1. */
    AMP_FB_DESC->pad1++;

    __stdio_init();
    main(0, 0);

    /* main returned (it never should): spin.  A visible HALT park would need
     * an inline-asm HALT; not worth a toolchain dependency for P3. */
    for (;;) {}
}

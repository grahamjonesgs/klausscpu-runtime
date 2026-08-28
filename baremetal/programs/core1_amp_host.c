/*
 * core1_amp_host.c — core-1 side of AMP P3: boot core 2 with the embedded
 * lwIP image, hand it LiteEth, and forward its console to the UART.
 *
 *   1. Hold core 2 (C2_CTRL.RUN=0).
 *   2. memcpy the core-2 flat image to the DDR text window (C2_TEXT_ENTRY)
 *      — ordinary cached stores — then CACHE FLUSH so DDR holds it
 *      (the coherency contract; core 2 reads DDR uncached / via its own
 *      read cache).
 *   3. C2_ETH_OWNER=1: core 2 owns the LiteEth windows from here on
 *      (this program never touches ethernet itself).
 *   4. START_PC=C2_TEXT_ENTRY, RUN=1.
 *   5. Forever: pop core 2's log FIFO to stdout; report if it parks.
 *
 * Build: make core1_amp_host   (builds core2.elf -> core2.bin -> core2_image.h
 * first).  Load over serial with klausscc; ping the board from the LAN.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "../../mmio.h"
#include "core2_image.h"      /* generated: core2_image[], core2_image_len */

static void cache_flush(void)
{
    REG_CACHE_CTRL = CACHE_CTRL_FLUSH;
    while (REG_CACHE_STATUS & 1u) {}
}

int main(void)
{
    printf("amp host: core-2 image %u bytes -> 0x%08x\n",
           (unsigned)core2_image_len, (unsigned)C2_TEXT_ENTRY);
    if (core2_image_len > C2_TEXT_SIZE - 0x20u) {
        printf("amp host: image too large for the text window\n");
        return 1;
    }

    REG_C2_CTRL = 0;                                  /* hold core 2 in reset */
    memcpy((void *)(uintptr_t)C2_TEXT_ENTRY, core2_image, core2_image_len);
    cache_flush();                                    /* push it down to DDR */

    REG_C2_ETH_OWNER = 1;
    printf("amp host: LiteEth -> core 2\n");

    REG_C2_START_PC = C2_TEXT_ENTRY;
    REG_C2_CTRL     = 1;
    printf("amp host: core 2 running; forwarding its console\n");

    for (;;) {
        uint64_t v = REG_C2_LOG;
        if (v & 0x100u) {
            putchar((int)(v & 0xFFu));
            if ((v & 0xFFu) == '\n') fflush(stdout);
            REG_C2_LOG = 0;                           /* pop */
            continue;
        }
        uint64_t c = REG_C2_CTRL;
        if (c & 2u) {
            printf("\namp host: core 2 PARKED kind=%u pc=0x%08x\n",
                   (unsigned)((c >> 2) & 7u), (unsigned)REG_C2_PARK_PC);
            break;
        }
    }
    return 0;
}

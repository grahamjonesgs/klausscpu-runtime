// test_irq_ext — board test for the Ethernet interrupt (source 2) and the
// I-cache invalidate (CACHE_CTRL[3]) added in KlaussCPU M13.
//
//  T1  INT_PENDING[2] follows the MAC's TX event while masked (level).
//  T2  An unmasked TX-done event dispatches the source-2 ISR exactly once;
//      the ISR's W1C leaves INT_PENDING[2] clear.
//  T3  ICACHE_INV: a cached function is rewritten in DDR by the blitter (DMA —
//      not snooped by the core). Without the fence the stale copy still runs
//      (informational: an unrelated eviction can refresh it); after
//      icache_invalidate() the new code must run.
#include <stdint.h>
#include <stdio.h>
#include "../../mmio.h"
#include "../../src/eth.h"

#define BLIT_BASE        (MMIO_BASE + 0x000E0000u)
#define REG_BLIT_CTRL    (*(volatile uint32_t *)(BLIT_BASE + 0x0000u))
#define REG_BLIT_STATUS  (*(volatile uint32_t *)(BLIT_BASE + 0x0008u))
#define REG_BLIT_DST     (*(volatile uint32_t *)(BLIT_BASE + 0x0010u))
#define REG_BLIT_DSTRIDE (*(volatile uint32_t *)(BLIT_BASE + 0x0018u))
#define REG_BLIT_SRC     (*(volatile uint32_t *)(BLIT_BASE + 0x0020u))
#define REG_BLIT_SSTRIDE (*(volatile uint32_t *)(BLIT_BASE + 0x0028u))
#define REG_BLIT_WIDTH   (*(volatile uint32_t *)(BLIT_BASE + 0x0030u))
#define REG_BLIT_HEIGHT  (*(volatile uint32_t *)(BLIT_BASE + 0x0038u))

static int pass, fail;
static void check(const char *name, int ok) {
    printf("%-34s %s\n", name, ok ? "PASS" : "FAIL");
    if (ok) pass++; else fail++;
}

// ── T1/T2: Ethernet interrupt ────────────────────────────────────────────
volatile uint32_t tx_irqs, rx_irqs;

void eth_isr_c(void) {
    if (REG_ETH_TX_EV_PENDING & 1u) { REG_ETH_TX_EV_PENDING = 1u; tx_irqs++; }
    if (REG_ETH_RX_EV_PENDING & 1u) { REG_ETH_RX_EV_PENDING = 1u; rx_irqs++; }
}

// Vector target: save the caller-saved registers, run the C handler, IRET
// (which restores PC, flags and INT_MASK).
__asm__(
    "  .text\n  .p2align 3\n  .globl eth_isr\n"
    "eth_isr:\n"
    "  push r0\n  push r1\n  push r2\n  push r3\n  push r8\n  push r9\n"
    "  push r10\n  push r11\n  push r12\n  push r13\n  push r14\n"
    "  call eth_isr_c\n"
    "  pop r14\n  pop r13\n  pop r12\n  pop r11\n  pop r10\n  pop r9\n"
    "  pop r8\n  pop r3\n  pop r2\n  pop r1\n  pop r0\n"
    "  iret\n");
extern void eth_isr(void);

static const uint8_t frame[60] = {
    0xff,0xff,0xff,0xff,0xff,0xff, 0x02,0x4b,0x43,0x00,0x00,0x01, 0x88,0xb5,
    'K','l','a','u','s','s','C','P','U',' ','I','R','Q',' ','t','e','s','t'};

static void wait_ms(uint32_t ms) {
    uint64_t t0 = REG_CLOCK_MS;
    while (REG_CLOCK_MS - t0 < ms) {}
}

static void test_eth_irq(void) {
    eth_init();
    REG_ETH_RX_EV_ENABLE  = 0;          // TX events only: deterministic
    REG_ETH_TX_EV_ENABLE  = 1;
    REG_ETH_TX_EV_PENDING = 1;
    REG_ETH_RX_EV_PENDING = 1;

    // T1: masked — the level shows in INT_PENDING[2] and clears on W1C.
    REG_INT_MASK = 0;
    eth_tx(frame, sizeof frame);
    int seen = 0;
    for (int i = 0; i < 100 && !seen; i++) { wait_ms(1); seen = (REG_INT_PEND >> INT_SRC_ETH) & 1u; }
    REG_ETH_TX_EV_PENDING = 1;
    wait_ms(1);
    check("T1 INT_PENDING[2] level (masked)", seen && !((REG_INT_PEND >> INT_SRC_ETH) & 1u));

    // T2: unmasked — one TX-done event, one ISR run.
    tx_irqs = rx_irqs = 0;
    REG_INT_VEC(INT_SRC_ETH) = (uint32_t)(uintptr_t)eth_isr;
    REG_INT_MASK = 1u << INT_SRC_ETH;
    eth_tx(frame, sizeof frame);
    for (int i = 0; i < 100 && !tx_irqs; i++) wait_ms(1);
    wait_ms(5);
    REG_INT_MASK = 0;
    REG_INT_VEC(INT_SRC_ETH) = 0;
    printf("   tx_irqs=%lu rx_irqs=%lu\n", (unsigned long)tx_irqs, (unsigned long)rx_irqs);
    check("T2 TX-done dispatches source 2 once",
          tx_irqs == 1 && !((REG_INT_PEND >> INT_SRC_ETH) & 1u));
}

// ── T3: I-cache invalidate against a DMA code write ───────────────────────
// f: return 1 (SETR r12,1 ; RET) padded to one 16-byte line.
__asm__(
    "  .text\n  .p2align 5\n  .globl icf\n"
    "icf:\n  setr r12, 1\n  ret\n  nop\n  nop\n  nop\n  nop\n  nop\n  nop\n");
extern long icf(void);
// The replacement: return 2. Staged as data, copied over icf by the blitter.
static const uint32_t icf2[8] __attribute__((aligned(32))) = {
    0x4BD02C00u /* SETR.S r12,2 */, 0x65800000u /* RET */,
    0x6C000000u, 0x6C000000u, 0x6C000000u, 0x6C000000u, 0x6C000000u, 0x6C000000u };

static void cache_mnt(uint32_t op) {
    REG_CACHE_CTRL = op;
    while (REG_CACHE_STATUS & CACHE_STATUS_MNT_BUSY) {}
}

static void test_icache_inv(void) {
    long first = icf();                              // caches icf's line
    cache_mnt(CACHE_CTRL_FLUSH);                     // DDR has icf2; all lines clean
    REG_BLIT_STATUS  = 2u;                           // clear DONE
    REG_BLIT_SRC     = (uint32_t)(uintptr_t)icf2;
    REG_BLIT_DST     = (uint32_t)(uintptr_t)icf;
    REG_BLIT_SSTRIDE = 32u;  REG_BLIT_DSTRIDE = 32u;
    REG_BLIT_WIDTH   = 16u;  REG_BLIT_HEIGHT  = 1u;  // 16 px x 2 B = 32 bytes
    REG_BLIT_CTRL    = 0x3u;                         // OP=COPY | START
    while (!(REG_BLIT_STATUS & 2u)) {}
    REG_BLIT_STATUS  = 2u;
    cache_mnt(CACHE_CTRL_INVALIDATE);                // drop the (clean, stale) D-cache copy
    long nofence = icf();                            // I-cache may still hold the old code
    icache_invalidate();
    long fenced = icf();
    printf("   first=%ld before-fence=%ld after-fence=%ld (%s)\n", first, nofence, fenced,
           nofence == 1 ? "stale until fenced" : "already refreshed");
    check("T3 ICACHE_INV exposes DMA-written code", first == 1 && fenced == 2);
}

int main(void) {
    printf("\n=== test_irq_ext: ETH IRQ + ICACHE_INV ===\n");
    test_icache_inv();
    test_eth_irq();
    printf("Results: %d pass, %d fail\n", pass, fail);
    return 0;
}

// ethernetif.c — lwIP netif driver for KlaussCPU LiteEth MAC.
//
// Connects lwIP's pbuf layer to the LiteEth slot-SRAM MMIO interface.
// Called from the main loop (NO_SYS=1):
//
//   ethernetif_input(&netif);   // drain one pending RX frame into lwIP
//   sys_check_timeouts();        // run ARP/TCP/DHCP timers
//
// Once the RX interrupt is wired (Phase 6), call ethernetif_input() from
// the eth ISR or a semaphore-woken task instead of polling.

#include "lwip/opt.h"
#include "lwip/def.h"
#include "lwip/mem.h"
#include "lwip/pbuf.h"
#include "lwip/stats.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "netif/ethernet.h"
#include "../mmio.h"
#include "../src/eth.h"   // g_eth_mac, sram helpers via eth.h declarations
#include <string.h>

// Profiling hooks: defined by core2/lwipopts.h (c2_prof.h) in the AMP core-2
// build only; no-ops everywhere else (the bucket names are never evaluated).
#ifndef PROF_PUSH
#define PROF_PUSH(b) ((void)0)
#define PROF_POP()   ((void)0)
#endif

// Always transmit from slot 0.  Slot 1 is reserved for a future DMA path.
// TX_READY means "room in the 2-entry TX FIFO", not "wire idle", so we wait
// for TX_LEVEL==0 (FIFO empty) before each send to prevent back-to-back
// double-transmission of the same frame.

// ── Volatile SRAM copy helpers ────────────────────────────────────────────
// Standard memcpy rejects volatile pointers; use explicit byte loops.

// Word-wide SRAM copies.  On this fetch-bound core the per-byte loop overhead
// dominates packet processing, so move 32 bits per iteration (LDIDX32/STIDX32)
// when both ends are 4-aligned, with a byte tail / unaligned fallback.  This is
// a raw word copy (read u32 → write the same u32), so the byte layout — and
// therefore the on-wire byte order — is preserved regardless of endianness.

static inline void sram_copy_from(uint8_t *dst, volatile const uint8_t *src, uint32_t n) {
    if ((((uintptr_t)dst | (uintptr_t)src) & 3u) == 0u) {
        volatile const uint32_t *s = (volatile const uint32_t *)(const void *)src;
        uint32_t *d = (uint32_t *)(void *)dst;
        uint32_t w = n >> 2;
        for (uint32_t i = 0; i < w; i++) d[i] = s[i];
        for (uint32_t i = w << 2; i < n; i++) dst[i] = src[i];
    } else {
        for (uint32_t i = 0; i < n; i++) dst[i] = src[i];
    }
}

static inline void sram_copy_to(volatile uint8_t *dst, const uint8_t *src, uint32_t n) {
    if ((((uintptr_t)dst | (uintptr_t)src) & 3u) == 0u) {
        volatile uint32_t *d = (volatile uint32_t *)(void *)dst;
        const uint32_t *s = (const uint32_t *)(const void *)src;
        uint32_t w = n >> 2;
        for (uint32_t i = 0; i < w; i++) d[i] = s[i];
        for (uint32_t i = w << 2; i < n; i++) dst[i] = src[i];
    } else {
        for (uint32_t i = 0; i < n; i++) dst[i] = src[i];
    }
}

#ifdef ETH_TX_WIDE64
// TX copy as aligned 64-bit writes (the MMIO bridge splits each into two
// 32-bit Wishbone cycles), shift-merging a misaligned source.  Outgoing
// frames start 2 bytes off a 4-byte boundary — lwIP aligns the transport
// payload and the 54 header bytes sit in front — so sram_copy_to's 4-aligned
// test never passed and every byte went out as its own MMIO transaction.
// The merge may read up to 7 bytes past the source end, within the last
// aligned 8-byte word only (plain RAM, no MMU).  Little-endian, so the byte
// layout on the wire is unchanged.
static void sram_copy_to64(volatile uint8_t *dst, const uint8_t *src, uint32_t n) {
    while (n && (((uintptr_t)dst) & 7u)) { *dst++ = *src++; n--; }
    uint32_t w = n >> 3;
    volatile uint64_t *dw = (volatile uint64_t *)(void *)dst;
    unsigned k = (unsigned)(((uintptr_t)src) & 7u);
    if (k == 0) {
        const uint64_t *sw = (const uint64_t *)(const void *)src;
        for (uint32_t i = 0; i < w; i++) dw[i] = sw[i];
    } else {
        const uint64_t *sw = (const uint64_t *)(const void *)(src - k);
        unsigned sh = 8u * k;
        uint64_t lo = *sw++;
        for (uint32_t i = 0; i < w; i++) {
            uint64_t hi = *sw++;
            dw[i] = (lo >> sh) | (hi << (64u - sh));
            lo = hi;
        }
    }
    dst += w << 3; src += w << 3; n &= 7u;
    while (n--) *dst++ = *src++;
}
#define sram_copy_to sram_copy_to64
#endif

static void sram_to_pbuf(struct pbuf *p, volatile const uint8_t *src) {
    uint32_t offset = 0;
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        sram_copy_from((uint8_t *)q->payload, src + offset, q->len);
        offset += q->len;
    }
}

static void pbuf_to_sram(volatile uint8_t *dst, struct pbuf *p) {
    uint32_t offset = 0;
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        sram_copy_to(dst + offset, (const uint8_t *)q->payload, q->len);
        offset += q->len;
    }
}

// ── low_level_init ────────────────────────────────────────────────────────

static err_t low_level_init(struct netif *netif) {
    netif->hwaddr_len = ETHARP_HWADDR_LEN;
    for (int i = 0; i < 6; i++) netif->hwaddr[i] = g_eth_mac[i];
    netif->mtu   = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;
    return ERR_OK;
}

#ifdef ETH_RX_QUEUE
// RX queue.  The MAC has only two RX slots, and lwIP takes far longer to
// process a frame than the next ones take to arrive: a peer sending 3+ frames
// back to back (e.g. ACKs + a request) loses the third, which costs it a
// ~200 ms retransmission timeout.  So frames are moved out of the slots as
// soon as possible — before each one is processed, and on every transmit
// (transmits happen inside processing) — into one FIFO.  Every frame goes
// through it, so lwIP still sees them in arrival order (TCP_QUEUE_OOSEQ is
// 0: out-of-order segments would simply be dropped).  Bounded below
// PBUF_POOL_SIZE so RX pbufs remain available.
#define RX_Q_LEN 6
static struct pbuf *rx_q[RX_Q_LEN];
static unsigned rx_q_head, rx_q_count;
static struct pbuf *low_level_input(struct netif *netif);

static void rx_drain(void) {
    while (rx_q_count < RX_Q_LEN) {
        struct pbuf *p = low_level_input(NULL);
        if (p == NULL) break;
        rx_q[(rx_q_head + rx_q_count) % RX_Q_LEN] = p;
        rx_q_count++;
    }
}
#endif

// ── low_level_output — pbuf chain → TX slot SRAM → kick MAC ──────────────

static err_t low_level_output(struct netif *netif, struct pbuf *p) {
    (void)netif;

    if (p->tot_len > (uint16_t)ETH_SLOT_SIZE) {
        LINK_STATS_INC(link.err);
        return ERR_BUF;
    }

#ifdef ETH_RX_QUEUE
    rx_drain();
#endif

    PROF_PUSH(PROF_TX_WAIT);
#ifdef ETH_TX_SAFE
    // Every frame goes through slot 0, so the previous frame must have left
    // it before we overwrite it: LiteEth's SRAM reader pops its command FIFO
    // only after reading the frame out, so LEVEL == 0 means slot 0 is free.
    // (TX_READY only means "FIFO not full" — with a fast copy that let us
    // rewrite slot 0 mid-transmit.)
    while (REG_ETH_TX_LEVEL != 0) {}
#else
    while (!REG_ETH_TX_READY) {}   // wait if TX FIFO full
#endif
    PROF_POP();

    PROF_PUSH(PROF_TX_COPY);
    pbuf_to_sram(ETH_TX_SLOT_PTR(0), p);
    PROF_POP();
    uint32_t tx_len = p->tot_len;
#ifdef ETH_TX_SAFE
    // Pad runts to the 60-byte Ethernet minimum: this MAC doesn't, and the
    // switch silently drops shorter frames — e.g. every 54-byte pure TCP ACK
    // (the Zephyr driver, eth_klausscpu.c, already pads for the same reason).
    if (tx_len < 60u) {
        volatile uint8_t *slot = ETH_TX_SLOT_PTR(0);
        for (uint32_t i = tx_len; i < 60u; i++) slot[i] = 0;
        tx_len = 60u;
    }
#endif
    REG_ETH_TX_SLOT   = 0;
    REG_ETH_TX_LENGTH = tx_len;
    REG_ETH_TX_START  = 1;

    LINK_STATS_INC(link.xmit);
    return ERR_OK;
}

// ── low_level_input — RX slot SRAM → pbuf chain ──────────────────────────
// Returns a freshly allocated pbuf chain, or NULL if nothing is pending.

static struct pbuf *low_level_input(struct netif *netif) {
    (void)netif;

    if (!(REG_ETH_RX_EV_PENDING & 1u)) return NULL;

    uint32_t slot = REG_ETH_RX_SLOT;
    uint32_t len  = REG_ETH_RX_LENGTH;

    if (len < 14 || len > ETH_SLOT_SIZE) {
        REG_ETH_RX_EV_PENDING = 1;   // W1C — discard garbage frame
        LINK_STATS_INC(link.err);
        return NULL;
    }

    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
    if (p == NULL) {
        REG_ETH_RX_EV_PENDING = 1;   // W1C — drop; we're out of pbufs
        LINK_STATS_INC(link.memerr);
        return NULL;
    }

    PROF_PUSH(PROF_RX_COPY);
    sram_to_pbuf(p, ETH_RX_SLOT_PTR(slot));
    PROF_POP();
    REG_ETH_RX_EV_PENDING = 1;   // W1C — release slot to MAC

    LINK_STATS_INC(link.recv);
    return p;
}

// ── ethernetif_input — called from polling loop ───────────────────────────
// Drains ALL pending RX frames and passes each up to the lwIP IP layer.
//
// Draining the whole LiteEth RX ring per call (rather than one frame) keeps
// the MAC's slot ring from overflowing under bursty bulk transfers — the
// dominant throughput limiter when polled from a NO_SYS main loop.  Each
// low_level_input() re-reads RX_EV_PENDING/RX_SLOT/RX_LENGTH and W1C-releases
// its slot, so the MAC advances to the next pending frame on every iteration.

void ethernetif_input(struct netif *netif) {
    struct pbuf *p;

#ifdef ETH_RX_QUEUE
    for (;;) {
        rx_drain();                 // newest frames join the tail first
        if (rx_q_count == 0) return;
        p = rx_q[rx_q_head];
        rx_q_head = (rx_q_head + 1) % RX_Q_LEN;
        rx_q_count--;
        if (netif->input(p, netif) != ERR_OK) pbuf_free(p);
    }
#endif

    while ((p = low_level_input(netif)) != NULL) {
        err_t err = netif->input(p, netif);
        if (err != ERR_OK) {
            LWIP_DEBUGF(NETIF_DEBUG, ("ethernetif_input: IP layer dropped frame %d\n",
                                      (int)err));
            pbuf_free(p);
        }
    }
}

// ── ethernetif_init — called once by netif_add() ─────────────────────────

err_t ethernetif_init(struct netif *netif) {
    LWIP_ASSERT("netif != NULL", netif != NULL);

#if LWIP_NETIF_HOSTNAME
    netif->hostname = "klausscpu";
#endif

    netif->name[0] = 'e';
    netif->name[1] = '0';
    netif->output     = etharp_output;   // IPv4 → ARP then low_level_output
    netif->linkoutput = low_level_output;

    return low_level_init(netif);
}

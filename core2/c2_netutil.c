// c2_netutil.c — AMP core 2 networking helpers: profiler storage, and fast
// replacements for the two byte-at-a-time hot paths lwIP uses here:
//
//   c2_chksum  (LWIP_CHKSUM)   — Internet checksum with 64-bit loads.  The
//                                stock LWIP_CHKSUM_ALGORITHM 1 reads one byte
//                                at a time (chosen because 16-bit loads read
//                                big-endian on this core); this uses only
//                                64-bit and 8-bit loads, so it avoids that too.
//   c2_memcpy  (MEMCPY/SMEMCPY) — 64-bit copy that shift-merges a misaligned
//                                source; libc memcpy runs ~10 cycles/byte here.
//
// Both are checked against byte-at-a-time references at boot
// (c2_netutil_selftest); on any mismatch they fall back to the slow paths.
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include "lwip/opt.h"
#include "c2_prof.h"

uint32_t c2_prof_ms[PROF_N];
uint8_t  c2_prof_stack[16];
int      c2_prof_sp;
uint64_t c2_prof_t;

u16_t lwip_standard_chksum(const void *dataptr, int len);

static int fast_chksum_ok;
static int fast_memcpy_ok;

/* Same result as LWIP_CHKSUM_ALGORITHM 1 (host-order, non-inverted sum with
 * pairing relative to the start).  Sums by *address* parity — even-address
 * bytes low, odd-address bytes high, i.e. little-endian 16-bit words — which
 * equals algorithm 1's result for an even start and its byte swap for an odd
 * start (RFC 1071 byte-order independence). */
static uint16_t chksum_fast(const void *dataptr, int len)
{
    const uint8_t *p = (const uint8_t *)dataptr;
    int odd_start = (int)((uintptr_t)p & 1u);
    uint64_t acc = 0;

    while (len > 0 && ((uintptr_t)p & 7u)) {
        acc += ((uintptr_t)p & 1u) ? ((uint64_t)*p << 8) : (uint64_t)*p;
        p++; len--;
    }
    while (len >= 8) {
        uint64_t w = *(const uint64_t *)(const void *)p;
        acc += (w & 0xFFFFFFFFu) + (w >> 32);
        p += 8; len -= 8;
    }
    while (len > 0) {
        acc += ((uintptr_t)p & 1u) ? ((uint64_t)*p << 8) : (uint64_t)*p;
        p++; len--;
    }
    acc = (acc & 0xFFFFFFFFu) + (acc >> 32);
    acc = (acc & 0xFFFFFFFFu) + (acc >> 32);
    acc = (acc & 0xFFFFu) + (acc >> 16);
    acc = (acc & 0xFFFFu) + (acc >> 16);
    acc = (acc & 0xFFFFu) + (acc >> 16);
    uint16_t r = (uint16_t)acc;
    return odd_start ? (uint16_t)((r << 8) | (r >> 8)) : r;
}

uint16_t c2_chksum(const void *dataptr, int len)
{
    PROF_PUSH(PROF_CHKSUM);
    uint16_t r = fast_chksum_ok ? chksum_fast(dataptr, len)
                                : lwip_standard_chksum(dataptr, len);
    PROF_POP();
    return r;
}

/* 64-bit copy.  Aligns the destination, then either copies words directly or
 * builds each output word from two aligned source words (little-endian shift
 * merge).  The merge may read up to 7 bytes past the source end, but only
 * within the last aligned 8-byte word it touches — harmless here (no MMU,
 * plain RAM). */
static void *memcpy_fast(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    if (n >= 32) {
        while ((uintptr_t)d & 7u) { *d++ = *s++; n--; }
        size_t w = n >> 3;
        uint64_t *dw = (uint64_t *)(void *)d;
        unsigned k = (unsigned)((uintptr_t)s & 7u);
        if (k == 0) {
            const uint64_t *sw = (const uint64_t *)(const void *)s;
            for (size_t i = 0; i < w; i++) dw[i] = sw[i];
        } else {
            const uint64_t *sw = (const uint64_t *)(const void *)(s - k);
            unsigned sh = 8u * k;
            uint64_t lo = *sw++;
            for (size_t i = 0; i < w; i++) {
                uint64_t hi = *sw++;
                dw[i] = (lo >> sh) | (hi << (64u - sh));
                lo = hi;
            }
        }
        d += w << 3; s += w << 3; n &= 7u;
    }
    while (n--) *d++ = *s++;
    return dst;
}

/* Copy + Internet checksum of the copied bytes in one pass (lwIP's
 * LWIP_CHKSUM_COPY, used by tcp_write with LWIP_CHECKSUM_ON_COPY).  Same
 * structure as memcpy_fast; each 64-bit word is summed as it is stored.
 * Address parity is taken from dst (the sum is start-relative, so either
 * buffer works; dst is the one we align). */
static uint16_t chksum_copy_fast(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    int odd_start = (int)((uintptr_t)d & 1u);
    uint64_t acc = 0;

    while (n && ((uintptr_t)d & 7u)) {
        uint8_t b = *s++;
        acc += ((uintptr_t)d & 1u) ? ((uint64_t)b << 8) : (uint64_t)b;
        *d++ = b; n--;
    }
    size_t w = n >> 3;
    uint64_t *dw = (uint64_t *)(void *)d;
    unsigned k = (unsigned)((uintptr_t)s & 7u);
    if (k == 0) {
        const uint64_t *sw = (const uint64_t *)(const void *)s;
        for (size_t i = 0; i < w; i++) {
            uint64_t v = sw[i];
            dw[i] = v;
            acc += (v & 0xFFFFFFFFu) + (v >> 32);
        }
    } else {
        const uint64_t *sw = (const uint64_t *)(const void *)(s - k);
        unsigned sh = 8u * k;
        uint64_t lo = *sw++;
        for (size_t i = 0; i < w; i++) {
            uint64_t hi = *sw++;
            uint64_t v = (lo >> sh) | (hi << (64u - sh));
            dw[i] = v;
            acc += (v & 0xFFFFFFFFu) + (v >> 32);
            lo = hi;
        }
    }
    d += w << 3; s += w << 3; n &= 7u;
    while (n--) {
        uint8_t b = *s++;
        acc += ((uintptr_t)d & 1u) ? ((uint64_t)b << 8) : (uint64_t)b;
        *d++ = b;
    }
    acc = (acc & 0xFFFFFFFFu) + (acc >> 32);
    acc = (acc & 0xFFFFFFFFu) + (acc >> 32);
    acc = (acc & 0xFFFFu) + (acc >> 16);
    acc = (acc & 0xFFFFu) + (acc >> 16);
    acc = (acc & 0xFFFFu) + (acc >> 16);
    uint16_t r = (uint16_t)acc;
    return odd_start ? (uint16_t)((r << 8) | (r >> 8)) : r;
}

static int fast_chksum_copy_ok;

uint16_t c2_chksum_copy(void *dst, const void *src, uint16_t len)
{
    if (fast_chksum_copy_ok) {
        PROF_PUSH(PROF_TCP_WRITE);
        uint16_t r = chksum_copy_fast(dst, src, len);
        PROF_POP();
        return r;
    }
    c2_memcpy(dst, src, len);
    return c2_chksum(dst, len);
}

static void *memcpy_bytes(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *c2_memcpy(void *dst, const void *src, size_t n)
{
    return fast_memcpy_ok ? memcpy_fast(dst, src, n) : memcpy_bytes(dst, src, n);
}

/* ── boot self-test ───────────────────────────────────────────────────── */
static uint8_t st_a[1600] __attribute__((aligned(8)));
static uint8_t st_b[1600] __attribute__((aligned(8)));

void c2_netutil_selftest(void)
{
    uint32_t x = 0x12345678u;
    for (int i = 0; i < (int)sizeof(st_a); i++) {
        x = x * 1103515245u + 12345u;
        st_a[i] = (uint8_t)(x >> 16);
    }
    /* A few runs of 0xFF / 0x00 so carries and folding are exercised. */
    for (int i = 100; i < 300; i++) st_a[i] = 0xFF;
    for (int i = 400; i < 420; i++) st_a[i] = 0x00;

    unsigned bad = 0, cases = 0;
    static const int lens[] = { 0, 1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 32, 33, 54, 63,
                                64, 65, 255, 1460, 1500, 1514 };
    for (int off = 0; off < 8; off++) {
        for (unsigned li = 0; li < sizeof(lens) / sizeof(lens[0]); li++) {
            int len = lens[li];
            if (off + len > (int)sizeof(st_a)) continue;
            cases++;
            if (chksum_fast(st_a + off, len) != lwip_standard_chksum(st_a + off, len)) bad++;
        }
    }
    fast_chksum_ok = (bad == 0);
    printf("core2: chksum self-test %s\n", bad ? "FAIL (using lwIP routine)" : "pass");
    printf("core2: chksum cases=%u bad=%u\n", cases, bad);

    bad = 0; cases = 0;
    for (int so = 0; so < 8; so++) {
        for (int dof = 0; dof < 8; dof++) {
            for (unsigned li = 0; li < sizeof(lens) / sizeof(lens[0]); li++) {
                int len = lens[li];
                if (so + len > (int)sizeof(st_a) - 8 || dof + len > (int)sizeof(st_b) - 8) continue;
                cases++;
                for (int i = 0; i < (int)sizeof(st_b); i++) st_b[i] = 0xA5;
                memcpy_fast(st_b + dof, st_a + so, (size_t)len);
                for (int i = 0; i < (int)sizeof(st_b); i++) {
                    uint8_t want = (i >= dof && i < dof + len) ? st_a[so + i - dof] : 0xA5;
                    if (st_b[i] != want) { bad++; break; }
                }
            }
        }
    }
    fast_memcpy_ok = (bad == 0);
    printf("core2: memcpy self-test %s\n", bad ? "FAIL (using byte copy)" : "pass");
    printf("core2: memcpy cases=%u bad=%u\n", cases, bad);

    /* Copy+checksum: bytes must match, and the sum must equal lwIP's
     * reference checksum of the destination. */
    bad = 0; cases = 0;
    for (int so = 0; so < 8; so++) {
        for (int dof = 0; dof < 8; dof++) {
            for (unsigned li = 0; li < sizeof(lens) / sizeof(lens[0]); li++) {
                int len = lens[li];
                if (so + len > (int)sizeof(st_a) - 8 || dof + len > (int)sizeof(st_b) - 8) continue;
                cases++;
                for (int i = 0; i < (int)sizeof(st_b); i++) st_b[i] = 0xA5;
                uint16_t got = chksum_copy_fast(st_b + dof, st_a + so, (size_t)len);
                int ok = (got == lwip_standard_chksum(st_b + dof, len));
                for (int i = 0; ok && i < (int)sizeof(st_b); i++) {
                    uint8_t want = (i >= dof && i < dof + len) ? st_a[so + i - dof] : 0xA5;
                    if (st_b[i] != want) ok = 0;
                }
                if (!ok) bad++;
            }
        }
    }
    fast_chksum_copy_ok = (bad == 0) && fast_chksum_ok;
    printf("core2: chksum_copy self-test %s\n", fast_chksum_copy_ok ? "pass" : "FAIL (two-pass)");
    printf("core2: chksum_copy cases=%u bad=%u\n", cases, bad);
}

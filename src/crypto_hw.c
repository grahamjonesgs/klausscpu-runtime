/* crypto_hw.c — KlaussCPU hardware crypto MMIO drivers.
 *
 * Implements the API declared in crypto_hw.h.  All functions poll BUSY
 * (no IRQ-driven operation).  Callers must serialize concurrent access
 * with a mutex in multi-threaded environments.
 */

#include "../crypto_hw.h"
#include "../mmio.h"
#include <string.h>

/* ── Internal helpers ────────────────────────────────────────────────────── */

static inline void aes_wait(void) {
    while (REG_AES_STATUS & AES_STATUS_BUSY) {}
}

static inline void sha_wait(void) {
    while (!(REG_SHA_STATUS & SHA_STATUS_DONE)) {}
}

static inline void hmac_wait(void) {
    while (REG_HMAC_STATUS & HMAC_STATUS_BUSY) {}
}

/* A CTR block started by an AES_IN1 write: BUSY covers it, and DONE (cleared
 * by the write) is set when OUT is valid. */
static inline void aes_wait_out(void) {
    uint32_t st;
    do { st = REG_AES_STATUS; } while ((st & AES_STATUS_BUSY) || !(st & AES_STATUS_DONE));
}

/* After RESTORE / ZERO (and RESTORE|START), BUSY also covers a queued request. */
static inline void sha_wait_idle(void) {
    while (REG_SHA_STATUS & SHA_STATUS_BUSY) {}
}

/* 64-bit loads/stores of byte buffers.  The build is -fno-builtin, so a plain
 * memcpy of 8 bytes is a library call; __builtin_memcpy is inlined (as byte
 * accesses unless the buffer is known to be aligned). */
typedef uint64_t __attribute__((may_alias, aligned(8))) u64_aligned;

static inline uint64_t ld64(const uint8_t *p) {
    if (((uintptr_t)p & 7u) == 0) return *(const u64_aligned *)p;
    uint64_t v;
    __builtin_memcpy(&v, p, 8);
    return v;
}

static inline void st64(uint8_t *p, uint64_t v) {
    if (((uintptr_t)p & 7u) == 0) { *(u64_aligned *)p = v; return; }
    __builtin_memcpy(p, &v, 8);
}

/* ── Capabilities ────────────────────────────────────────────────────────── */

/* The bitstream cannot change under a running program, so read each once. */
uint32_t aes_hw_caps(void) {
    static int read;
    static uint32_t caps;
    if (!read) { caps = REG_AES_CAP; read = 1; }
    return caps;
}

uint32_t sha_hw_caps(void) {
    static int read;
    static uint32_t caps;
    if (!read) { caps = REG_SHA_CAP; read = 1; }
    return caps;
}

/* Write 64 bytes (8 × uint64_t) to SHA_BLOCK0..7. */
static void sha_write_block(const uint8_t *data) {
    volatile uint64_t *blk = (volatile uint64_t *)(SHA_BASE + 0x010u);
    for (int i = 0; i < 8; i++) {
        uint64_t w;
        memcpy(&w, data + i * 8, 8);
        blk[i] = w;
    }
}

/* Read 32-byte SHA digest from SHA_DIGEST0..3 into a byte array. */
static void sha_read_digest(uint8_t out[32]) {
    volatile uint64_t *dig = (volatile uint64_t *)(SHA_BASE + 0x050u);
    for (int i = 0; i < 4; i++) {
        uint64_t w = dig[i];
        memcpy(out + i * 8, &w, 8);
    }
}

/* ── Internal SHA-256 block processing (used by sha256_hw_*) ─────────────── */

/* Process any number of complete 64-byte blocks already staged in hardware
 * (assumes SHA hardware is already INIT'd). */
static void sha_process_full_blocks(const uint8_t *data, size_t n_blocks) {
    for (size_t i = 0; i < n_blocks; i++) {
        sha_write_block(data + i * 64);
        REG_SHA_CTRL = SHA_CTRL_START;
        sha_wait();
    }
}

/* Finalize a SHA-256 computation.
 * partial: the remaining bytes (< 64) of the message.
 * partial_len: number of bytes in partial.
 * total_bits: total message length in bits.
 * out: 32-byte digest output.
 *
 * SHA-256 padding: append 0x80, zeros, then 64-bit BE bit count in last 8 bytes.
 * If partial_len >= 56 we need two padding blocks (the 0x80 and zeros don't
 * leave room for the 8-byte length field in the same block). */
static void sha_finalize(const uint8_t *partial, size_t partial_len,
                         uint64_t total_bits, uint8_t out[32]) {
    uint8_t pad[128];
    memset(pad, 0, sizeof(pad));
    if (partial_len) memcpy(pad, partial, partial_len);
    pad[partial_len] = 0x80;

    /* Write 64-bit BE bit length into last 8 bytes of the last 64-byte pad block. */
    size_t padblocks = (partial_len < 56) ? 1 : 2;
    size_t lpos = padblocks * 64 - 8;
    pad[lpos]   = (uint8_t)(total_bits >> 56);
    pad[lpos+1] = (uint8_t)(total_bits >> 48);
    pad[lpos+2] = (uint8_t)(total_bits >> 40);
    pad[lpos+3] = (uint8_t)(total_bits >> 32);
    pad[lpos+4] = (uint8_t)(total_bits >> 24);
    pad[lpos+5] = (uint8_t)(total_bits >> 16);
    pad[lpos+6] = (uint8_t)(total_bits >>  8);
    pad[lpos+7] = (uint8_t)(total_bits      );

    if (padblocks == 2) {
        sha_write_block(pad);
        REG_SHA_CTRL = SHA_CTRL_START;
        sha_wait();
    }
    /* Final block — software has already written the length into pad[], so
     * SHA_CTRL_START alone completes the hash. No LAST bit or BITLEN register. */
    sha_write_block(pad + (padblocks - 1) * 64);
    REG_SHA_CTRL = SHA_CTRL_START;
    sha_wait();

    sha_read_digest(out);
}

/* ── AES-128 ECB ─────────────────────────────────────────────────────────── */

void aes_hw_set_key(uint64_t key_lo, uint64_t key_hi) {
    REG_AES_KEY0 = key_lo;
    REG_AES_KEY1 = key_hi;
    REG_AES_CTRL = AES_CTRL_KEY_LOAD;
    aes_wait();
}

void aes_hw_encrypt(uint64_t in_lo, uint64_t in_hi,
                    uint64_t *out_lo, uint64_t *out_hi) {
    REG_AES_IN0 = in_lo;
    REG_AES_IN1 = in_hi;
    REG_AES_CTRL = AES_CTRL_GO | AES_CTRL_ENC;
    aes_wait();
    *out_lo = REG_AES_OUT0;
    *out_hi = REG_AES_OUT1;
}

void aes_hw_decrypt(uint64_t in_lo, uint64_t in_hi,
                    uint64_t *out_lo, uint64_t *out_hi) {
    REG_AES_IN0 = in_lo;
    REG_AES_IN1 = in_hi;
    REG_AES_CTRL = AES_CTRL_GO; /* ENC=0 → decrypt */
    aes_wait();
    *out_lo = REG_AES_OUT0;
    *out_hi = REG_AES_OUT1;
}

/* ── AES-128 CTR ─────────────────────────────────────────────────────────── */

static uint64_t bswap64(uint64_t v); /* defined below (GCM section) */

void aes_ctr_hw(uint8_t ctr[16], const uint8_t *in, uint8_t *out, size_t len,
                int inc32, uint8_t ks_tail[16]) {
    aes_wait();
    REG_AES_CTR0 = ld64(ctr);
    REG_AES_CTR1 = ld64(ctr + 8);
    REG_AES_MODE = AES_MODE_CTR | (inc32 ? AES_MODE_INC32 : 0u);

    while (len >= 16) {
        REG_AES_IN0 = ld64(in);
        REG_AES_IN1 = ld64(in + 8);          /* starts the block */
        aes_wait_out();
        st64(out,     REG_AES_OUT0);
        st64(out + 8, REG_AES_OUT1);
        in += 16; out += 16; len -= 16;
    }
    if (len > 0) {
        /* Zero-padded input: OUT beyond len is the bare keystream. */
        uint8_t blk[16] = {0}, o[16];
        memcpy(blk, in, len);
        REG_AES_IN0 = ld64(blk);
        REG_AES_IN1 = ld64(blk + 8);
        aes_wait_out();
        st64(o,     REG_AES_OUT0);
        st64(o + 8, REG_AES_OUT1);
        memcpy(out, o, len);
        if (ks_tail)
            for (int i = 0; i < 16; i++) ks_tail[i] = o[i] ^ blk[i];
    }

    st64(ctr,     REG_AES_CTR0);
    st64(ctr + 8, REG_AES_CTR1);
    REG_AES_MODE = 0;
}

void aes_ctr(uint64_t nonce_lo, uint64_t nonce_hi,
             const uint8_t *in, uint8_t *out, size_t len) {
    if (aes_hw_caps() & AES_CAP_CTR) {
        uint8_t ctr[16];
        st64(ctr, nonce_lo);
        st64(ctr + 8, nonce_hi);
        aes_ctr_hw(ctr, in, out, len, 0, NULL);
        return;
    }

    uint64_t ctr_lo = nonce_lo, ctr_hi = nonce_hi;

    /* Full 16-byte blocks. */
    while (len >= 16) {
        uint64_t ks_lo, ks_hi;
        aes_hw_encrypt(ctr_lo, ctr_hi, &ks_lo, &ks_hi);

        uint64_t a, b;
        memcpy(&a, in,      8);
        memcpy(&b, in + 8,  8);
        a ^= ks_lo;
        b ^= ks_hi;
        memcpy(out,     &a, 8);
        memcpy(out + 8, &b, 8);

        /* 128-bit BIG-ENDIAN counter increment (NIST SP 800-38A).
         * The counter block is a big-endian integer, so its least-significant
         * byte is byte 15 — the top byte of ctr_hi.  Increment via byte-swapped
         * arithmetic (bytes 8-15 in ctr_hi, carrying into bytes 0-7 in ctr_lo).
         * The previous `ctr_lo++` bumped byte 0 (the MOST-significant byte), so
         * only block 0 (no increment) was correct; blocks 1+ used a bad counter. */
        uint64_t hi_be = bswap64(ctr_hi) + 1;
        ctr_hi = bswap64(hi_be);
        if (hi_be == 0)                        /* carried out of bytes 8-15 */
            ctr_lo = bswap64(bswap64(ctr_lo) + 1);
        in  += 16; out += 16; len -= 16;
    }

    /* Tail: generate one keystream block, XOR remaining bytes. */
    if (len > 0) {
        uint64_t ks_lo, ks_hi;
        aes_hw_encrypt(ctr_lo, ctr_hi, &ks_lo, &ks_hi);
        uint8_t ks_bytes[16];
        memcpy(ks_bytes,     &ks_lo, 8);
        memcpy(ks_bytes + 8, &ks_hi, 8);
        for (size_t i = 0; i < len; i++)
            out[i] = in[i] ^ ks_bytes[i];
    }
}

void aes_ctr_full(uint64_t key_lo, uint64_t key_hi,
                  uint64_t nonce_lo, uint64_t nonce_hi,
                  const uint8_t *in, uint8_t *out, size_t len) {
    aes_hw_set_key(key_lo, key_hi);
    aes_ctr(nonce_lo, nonce_hi, in, out, len);
}

/* ── AES-GCM (raw GHASH interface per MMIO_MAP.md) ───────────────────────── */

static uint64_t bswap64(uint64_t v) {
    return ((v & 0xFF00000000000000ULL) >> 56) |
           ((v & 0x00FF000000000000ULL) >> 40) |
           ((v & 0x0000FF0000000000ULL) >> 24) |
           ((v & 0x000000FF00000000ULL) >>  8) |
           ((v & 0x00000000FF000000ULL) <<  8) |
           ((v & 0x0000000000FF0000ULL) << 24) |
           ((v & 0x000000000000FF00ULL) << 40) |
           ((v & 0x00000000000000FFULL) << 56);
}

/* Increment a 128-bit counter block; last 4 bytes = 32-bit big-endian counter. */
static void ctr_inc(uint8_t ctr[16]) {
    for (int i = 15; i >= 12; i--)
        if (++ctr[i]) break;
}

/* GHASH one (zero-padded) block of up to 16 bytes. */
static void ghash_block(const uint8_t *data, size_t len) {
    if (len == 16) {
        gcm_ghash_block(ld64(data), ld64(data + 8));
        return;
    }
    uint8_t blk[16] = {0};
    if (len) memcpy(blk, data, len > 16 ? 16 : len);
    uint64_t lo, hi;
    memcpy(&lo, blk,     8);
    memcpy(&hi, blk + 8, 8);
    gcm_ghash_block(lo, hi);
}

/* One pass of GCM on CTR mode with the GHASH unit running alongside: the AES
 * unit works on block i while GHASH absorbs the ciphertext of block i-1
 * (encrypt; OUT of the previous block) or of block i itself (decrypt; IN).
 * The two units are independent, so this overlaps them without the fused
 * mode of crypto bitstream 2.  On entry H is loaded, the AAD is hashed and the
 * CTR registers hold J0+1 with MODE = CTR|INC32. */
static void gcm_ctr_ghash(const uint8_t *in, uint8_t *out, size_t len, int dec) {
    uint64_t p0 = 0, p1 = 0;      /* the block GHASH still owes (encrypt) */
    int pending = 0;

    while (len >= 16) {
        uint64_t i0 = ld64(in), i1 = ld64(in + 8);
        REG_AES_IN0 = i0;
        REG_AES_IN1 = i1;                    /* starts the CTR block */
        if (dec) { p0 = i0; p1 = i1; pending = 1; }
        if (pending) {
            gcm_wait_done();
            REG_GCM_X0 = p0;
            REG_GCM_X1 = p1;
            REG_GCM_CTRL = GCM_CTRL_GO;      /* overlaps the AES block */
        }
        aes_wait_out();
        uint64_t o0 = REG_AES_OUT0, o1 = REG_AES_OUT1;
        st64(out, o0);
        st64(out + 8, o1);
        if (!dec) { p0 = o0; p1 = o1; pending = 1; }
        else pending = 0;
        in += 16; out += 16; len -= 16;
    }
    if (pending) {
        gcm_wait_done();
        REG_GCM_X0 = p0;
        REG_GCM_X1 = p1;
        REG_GCM_CTRL = GCM_CTRL_GO;
    }
    if (len > 0) {
        uint8_t blk[16] = {0}, o[16];
        memcpy(blk, in, len);
        REG_AES_IN0 = ld64(blk);
        REG_AES_IN1 = ld64(blk + 8);
        aes_wait_out();
        st64(o,     REG_AES_OUT0);
        st64(o + 8, REG_AES_OUT1);
        memcpy(out, o, len);
        /* GHASH the partial block zero-padded: ciphertext bytes only. */
        const uint8_t *c = dec ? blk : o;
        uint8_t h[16] = {0};
        memcpy(h, c, len);
        gcm_wait_done();
        gcm_ghash_block(ld64(h), ld64(h + 8));
    }
    gcm_wait_done();
    REG_AES_MODE = 0;
}

int aes_gcm_single_pass = 1;

static int gcm_ctr_caps(void) {
    return aes_gcm_single_pass && (aes_hw_caps() & (AES_CAP_CTR | AES_CAP_INC32)) == (AES_CAP_CTR | AES_CAP_INC32);
}

/* H, E_K(J0) and the AAD for a CTR-mode GCM; leaves CTR = J0+1 in CTR mode. */
static void gcm_ctr_setup(const uint8_t *iv, const uint8_t *aad, size_t aad_len,
                          uint64_t *m0, uint64_t *m1) {
    aes_wait();
    REG_AES_MODE = 0;
    REG_AES_IN0 = 0; REG_AES_IN1 = 0;
    REG_AES_CTRL = AES_CTRL_GO | AES_CTRL_ENC;
    aes_wait();
    REG_GCM_H0 = REG_AES_OUT0;
    REG_GCM_H1 = REG_AES_OUT1;
    REG_GCM_CTRL = GCM_CTRL_RESET;
    for (size_t off = 0; off < aad_len; off += 16)
        ghash_block(aad + off, (aad_len - off >= 16) ? 16 : (aad_len - off));

    /* E_K(J0) as a CTR block with IN = 0; CTR is then J0+1. */
    uint8_t j0[16] = {0};
    memcpy(j0, iv, 12); j0[15] = 0x01;
    REG_AES_CTR0 = ld64(j0);
    REG_AES_CTR1 = ld64(j0 + 8);
    REG_AES_MODE = AES_MODE_CTR | AES_MODE_INC32;
    REG_AES_IN0 = 0;
    REG_AES_IN1 = 0;
    aes_wait_out();
    *m0 = REG_AES_OUT0;
    *m1 = REG_AES_OUT1;
}

static void gcm_ctr_tag(size_t aad_len, size_t len, uint64_t m0, uint64_t m1,
                        uint8_t tag[16]) {
    gcm_ghash_block(bswap64((uint64_t)aad_len * 8), bswap64((uint64_t)len * 8));
    st64(tag,     REG_GCM_TAG0 ^ m0);
    st64(tag + 8, REG_GCM_TAG1 ^ m1);
}

void aes_gcm_encrypt(const uint8_t *iv,
                     const uint8_t *aad, size_t aad_len,
                     const uint8_t *pt, uint8_t *ct, size_t len,
                     uint8_t tag_out[16]) {
    if (gcm_ctr_caps()) {
        uint64_t m0, m1;
        gcm_ctr_setup(iv, aad, aad_len, &m0, &m1);
        gcm_ctr_ghash(pt, ct, len, 0);
        gcm_ctr_tag(aad_len, len, m0, m1, tag_out);
        return;
    }
    /* 1. Compute H = AES_K(0^128) and load into GHASH unit. */
    REG_AES_IN0 = 0; REG_AES_IN1 = 0;
    REG_AES_CTRL = AES_CTRL_GO | AES_CTRL_ENC;
    aes_wait();
    REG_GCM_H0 = REG_AES_OUT0;
    REG_GCM_H1 = REG_AES_OUT1;

    /* 2. Build J0 = IV ‖ 0x00000001; encrypt J0 for tag mask. */
    uint8_t j0[16] = {0};
    memcpy(j0, iv, 12); j0[15] = 0x01;
    uint64_t j0_lo, j0_hi;
    memcpy(&j0_lo, j0, 8); memcpy(&j0_hi, j0 + 8, 8);
    aes_hw_encrypt(j0_lo, j0_hi, &j0_lo, &j0_hi); /* j0_lo/hi now = keystream mask */

    /* 3. Reset GHASH accumulator. */
    REG_GCM_CTRL = GCM_CTRL_RESET;

    /* 4. GHASH over AAD (zero-padded to block boundary). */
    size_t off = 0;
    while (off < aad_len) {
        size_t n = (aad_len - off >= 16) ? 16 : (aad_len - off);
        ghash_block(aad + off, n);
        off += n;
    }

    /* 5. CTR encryption + GHASH of ciphertext. Counter starts at J0+1. */
    uint8_t ctr[16] = {0};
    memcpy(ctr, iv, 12); ctr[15] = 0x01;
    off = 0;
    if ((aes_hw_caps() & (AES_CAP_CTR | AES_CAP_INC32)) == (AES_CAP_CTR | AES_CAP_INC32)) {
        ctr[15] = 0x02;
        aes_ctr_hw(ctr, pt, ct, len, 1, NULL);
        for (; off < len; off += 16)
            ghash_block(ct + off, (len - off >= 16) ? 16 : (len - off));
    }
    while (off < len) {
        ctr_inc(ctr);
        uint64_t ctr_lo, ctr_hi, ks_lo, ks_hi;
        memcpy(&ctr_lo, ctr, 8); memcpy(&ctr_hi, ctr + 8, 8);
        aes_hw_encrypt(ctr_lo, ctr_hi, &ks_lo, &ks_hi);

        size_t n = (len - off >= 16) ? 16 : (len - off);
        uint8_t blk[16] = {0};
        memcpy(blk, pt + off, n);
        uint64_t p0, p1;
        memcpy(&p0, blk, 8); memcpy(&p1, blk + 8, 8);
        uint64_t c0 = p0 ^ ks_lo, c1 = p1 ^ ks_hi;
        uint8_t cblk[16];
        memcpy(cblk, &c0, 8); memcpy(cblk + 8, &c1, 8);
        memcpy(ct + off, cblk, n);
        ghash_block(cblk, n); /* GHASH over ciphertext bytes */
        off += n;
    }

    /* 6. GHASH length block: [len(A)]_64 || [len(C)]_64, each 64-bit BE, with
     * len(A) FIRST (block bytes 0-7).  gcm_ghash_block(lo,hi) loads lo->X0
     * (bytes 0-7), hi->X1 (bytes 8-15), so lo = bswap64(lenA), hi = bswap64(lenC).
     * (This was previously swapped — lenC in bytes 0-7 — which corrupted only
     * the tag; the ciphertext, which doesn't depend on the length block, passed.) */
    uint64_t len_a_bits = (uint64_t)aad_len * 8;
    uint64_t len_c_bits = (uint64_t)len     * 8;
    gcm_ghash_block(bswap64(len_a_bits), bswap64(len_c_bits));

    /* 7. tag = GHASH ^ AES_K(J0). */
    uint64_t t0 = REG_GCM_TAG0 ^ j0_lo;
    uint64_t t1 = REG_GCM_TAG1 ^ j0_hi;
    memcpy(tag_out,     &t0, 8);
    memcpy(tag_out + 8, &t1, 8);
}

int aes_gcm_decrypt(const uint8_t *iv,
                    const uint8_t *aad, size_t aad_len,
                    const uint8_t *ct, uint8_t *pt, size_t len,
                    const uint8_t tag_in[16]) {
    uint8_t computed_tag[16];
    if (gcm_ctr_caps()) {
        /* One pass: pt is written before the tag is known and wiped if it
         * doesn't match. */
        uint64_t m0, m1;
        gcm_ctr_setup(iv, aad, aad_len, &m0, &m1);
        gcm_ctr_ghash(ct, pt, len, 1);
        gcm_ctr_tag(aad_len, len, m0, m1, computed_tag);
        uint8_t diff = 0;
        for (int i = 0; i < 16; i++) diff |= computed_tag[i] ^ tag_in[i];
        if (diff) { memset(pt, 0, len); return -1; }
        return 0;
    }
    /* GCM decrypt: GHASH over CT, then CTR-decrypt CT→PT. */
    /* 1. H and J0. */
    REG_AES_IN0 = 0; REG_AES_IN1 = 0;
    REG_AES_CTRL = AES_CTRL_GO | AES_CTRL_ENC;
    aes_wait();
    REG_GCM_H0 = REG_AES_OUT0;
    REG_GCM_H1 = REG_AES_OUT1;

    uint8_t j0[16] = {0};
    memcpy(j0, iv, 12); j0[15] = 0x01;
    uint64_t j0_lo, j0_hi;
    memcpy(&j0_lo, j0, 8); memcpy(&j0_hi, j0 + 8, 8);
    aes_hw_encrypt(j0_lo, j0_hi, &j0_lo, &j0_hi);

    REG_GCM_CTRL = GCM_CTRL_RESET;

    /* 2. GHASH AAD. */
    size_t off = 0;
    while (off < aad_len) {
        size_t n = (aad_len - off >= 16) ? 16 : (aad_len - off);
        ghash_block(aad + off, n);
        off += n;
    }

    /* 3. GHASH CT. */
    off = 0;
    while (off < len) {
        size_t n = (len - off >= 16) ? 16 : (len - off);
        ghash_block(ct + off, n);
        off += n;
    }

    /* 4. Length block: [len(A)]_64 || [len(C)]_64, len(A) first (bytes 0-7). */
    gcm_ghash_block(bswap64((uint64_t)aad_len * 8), bswap64((uint64_t)len * 8));

    /* 5. Tag. */
    uint64_t t0 = REG_GCM_TAG0 ^ j0_lo;
    uint64_t t1 = REG_GCM_TAG1 ^ j0_hi;
    memcpy(computed_tag,     &t0, 8);
    memcpy(computed_tag + 8, &t1, 8);

    /* Constant-time compare. */
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= computed_tag[i] ^ tag_in[i];
    if (diff) { memset(pt, 0, len); return -1; }

    /* 6. CTR decrypt. */
    uint8_t ctr[16] = {0};
    memcpy(ctr, iv, 12); ctr[15] = 0x01;
    off = 0;
    if ((aes_hw_caps() & (AES_CAP_CTR | AES_CAP_INC32)) == (AES_CAP_CTR | AES_CAP_INC32)) {
        ctr[15] = 0x02;
        aes_ctr_hw(ctr, ct, pt, len, 1, NULL);
        off = len;
    }
    while (off < len) {
        ctr_inc(ctr);
        uint64_t ctr_lo, ctr_hi, ks_lo, ks_hi;
        memcpy(&ctr_lo, ctr, 8); memcpy(&ctr_hi, ctr + 8, 8);
        aes_hw_encrypt(ctr_lo, ctr_hi, &ks_lo, &ks_hi);
        size_t n = (len - off >= 16) ? 16 : (len - off);
        uint8_t blk[16] = {0};
        memcpy(blk, ct + off, n);
        uint64_t c0, c1;
        memcpy(&c0, blk, 8); memcpy(&c1, blk + 8, 8);
        c0 ^= ks_lo; c1 ^= ks_hi;
        memcpy(blk, &c0, 8); memcpy(blk + 8, &c1, 8);
        memcpy(pt + off, blk, n);
        off += n;
    }
    return 0;
}

/* ── SHA-256 (one-shot) ──────────────────────────────────────────────────── */

void sha256_hw(const uint8_t *msg, size_t len, uint8_t out[32]) {
    /* INIT resets H to the FIPS-180-4 IV; it is a state reset, NOT a block
     * compression, so it never asserts SHA_STATUS_DONE.  sha_wait() here spun
     * forever on the first SHA operation (the self-test hang).  The DONE
     * handshake belongs to SHA_CTRL_START, which every block below waits on. */
    REG_SHA_CTRL = SHA_CTRL_INIT;

    /* Full blocks. */
    size_t off = 0;
    while (off + 64 <= len) {
        sha_write_block(msg + off);
        REG_SHA_CTRL = SHA_CTRL_START;
        sha_wait();
        off += 64;
    }

    sha_finalize(msg + off, len - off, (uint64_t)len * 8, out);
}

/* ── SHA-256 (streaming) ─────────────────────────────────────────────────── */

void sha256_hw_init(sha256_ctx_t *ctx) {
    ctx->total_bytes = 0;
    ctx->buf_len     = 0;
    ctx->initialized = 1;
    /* INIT is a state reset, not a compression — no DONE handshake to wait on
     * (see sha256_hw); waiting here hangs. */
    REG_SHA_CTRL = SHA_CTRL_INIT;
}

void sha256_hw_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len) {
    ctx->total_bytes += len;

    /* If there are bytes in the partial block buffer, try to fill it first. */
    if (ctx->buf_len > 0) {
        size_t need = 64 - ctx->buf_len;
        size_t take = (len < need) ? len : need;
        memcpy(ctx->buf + ctx->buf_len, data, take);
        ctx->buf_len += (uint32_t)take;
        data += take; len -= take;
        if (ctx->buf_len == 64) {
            sha_write_block(ctx->buf);
            REG_SHA_CTRL = SHA_CTRL_START;
            sha_wait();
            ctx->buf_len = 0;
        }
    }

    /* Process complete blocks directly from input. */
    while (len >= 64) {
        sha_write_block(data);
        REG_SHA_CTRL = SHA_CTRL_START;
        sha_wait();
        data += 64; len -= 64;
    }

    /* Buffer any remaining bytes. */
    if (len > 0) {
        memcpy(ctx->buf, data, len);
        ctx->buf_len = (uint32_t)len;
    }
}

void sha256_hw_final(sha256_ctx_t *ctx, uint8_t out[32]) {
    sha_finalize(ctx->buf, ctx->buf_len,
                 ctx->total_bytes * 8, out);
    ctx->initialized = 0;
}

/* ── SHA-256 with state save/restore ─────────────────────────────────────── */

/* FIPS 180-4 IV as big-endian bytes, which is SHA_DIGEST/SHA_HIN's packing. */
static const uint8_t sha256_iv[32] = {
    0x6a, 0x09, 0xe6, 0x67, 0xbb, 0x67, 0xae, 0x85,
    0x3c, 0x6e, 0xf3, 0x72, 0xa5, 0x4f, 0xf5, 0x3a,
    0x51, 0x0e, 0x52, 0x7f, 0x9b, 0x05, 0x68, 0x8c,
    0x1f, 0x83, 0xd9, 0xab, 0x5b, 0xe0, 0xcd, 0x19,
};

/* Compress n_blocks 64-byte blocks onto state h: RESTORE, the blocks, save.
 * The first block goes with the RESTORE in one CTRL write. */
static void sha_r_blocks(uint64_t h[4], const uint8_t *data, size_t n_blocks) {
    if (n_blocks == 0) return;
    sha_wait_idle();
    for (int i = 0; i < 4; i++) REG_SHA_HIN(i) = h[i];
    for (size_t b = 0; b < n_blocks; b++) {
        for (int i = 0; i < 8; i++) REG_SHA_BLOCK(i) = ld64(data + b * 64 + i * 8);
        REG_SHA_CTRL = b == 0 ? (SHA_CTRL_RESTORE | SHA_CTRL_START) : SHA_CTRL_START;
        sha_wait_idle();
        sha_wait();
    }
    for (int i = 0; i < 4; i++) h[i] = REG_SHA_DIGEST(i);
}

void sha256_hw_rinit(sha256_hw_rctx_t *ctx) {
    for (int i = 0; i < 4; i++) ctx->h[i] = ld64(sha256_iv + i * 8);
    ctx->total_bytes = 0;
    ctx->buf_len = 0;
}

void sha256_hw_rinit_from(sha256_hw_rctx_t *ctx, const uint64_t h[4],
                          uint64_t prefix_bytes) {
    for (int i = 0; i < 4; i++) ctx->h[i] = h[i];
    ctx->total_bytes = prefix_bytes;
    ctx->buf_len = 0;
}

void sha256_hw_rupdate(sha256_hw_rctx_t *ctx, const uint8_t *data, size_t len) {
    ctx->total_bytes += len;
    if (ctx->buf_len > 0) {
        size_t take = 64 - ctx->buf_len;
        if (take > len) take = len;
        memcpy(ctx->buf + ctx->buf_len, data, take);
        ctx->buf_len += (uint32_t)take;
        data += take; len -= take;
        if (ctx->buf_len < 64) return;
        sha_r_blocks(ctx->h, ctx->buf, 1);
        ctx->buf_len = 0;
    }
    size_t n = len / 64;
    sha_r_blocks(ctx->h, data, n);
    data += n * 64; len -= n * 64;
    if (len > 0) {
        memcpy(ctx->buf, data, len);
        ctx->buf_len = (uint32_t)len;
    }
}

void sha256_hw_rfinal(sha256_hw_rctx_t *ctx, uint8_t out[32]) {
    uint8_t pad[128];
    size_t n = ctx->buf_len;
    uint64_t bits = ctx->total_bytes * 8;

    memset(pad, 0, sizeof(pad));
    memcpy(pad, ctx->buf, n);
    pad[n] = 0x80;
    size_t blocks = (n < 56) ? 1 : 2;
    for (int i = 0; i < 8; i++)
        pad[blocks * 64 - 1 - i] = (uint8_t)(bits >> (8 * i));
    sha_r_blocks(ctx->h, pad, blocks);
    for (int i = 0; i < 4; i++) st64(out + i * 8, ctx->h[i]);
    ctx->buf_len = 0;
}

/* ── HMAC-SHA-256 from software midstates ────────────────────────────────── */

void hmac_sha256_hw_rkey(hmac_sha256_hw_key_t *k, const uint8_t *key, size_t key_len) {
    uint8_t k0[64], blk[64];
    sha256_hw_rctx_t c;

    memset(k0, 0, sizeof(k0));
    if (key_len > 64) {
        sha256_hw_rinit(&c);
        sha256_hw_rupdate(&c, key, key_len);
        sha256_hw_rfinal(&c, k0);
    } else {
        memcpy(k0, key, key_len);
    }
    for (int i = 0; i < 64; i++) blk[i] = k0[i] ^ 0x36;
    sha256_hw_rinit(&c);
    sha_r_blocks(c.h, blk, 1);
    memcpy(k->ih, c.h, sizeof(k->ih));
    for (int i = 0; i < 64; i++) blk[i] = k0[i] ^ 0x5c;
    sha256_hw_rinit(&c);
    sha_r_blocks(c.h, blk, 1);
    memcpy(k->oh, c.h, sizeof(k->oh));
}

void hmac_sha256_hw_rmac(const hmac_sha256_hw_key_t *k, const uint8_t *msg,
                         size_t msg_len, uint8_t out[32]) {
    sha256_hw_rctx_t c;
    uint8_t inner[32];

    sha256_hw_rinit_from(&c, k->ih, 64);
    sha256_hw_rupdate(&c, msg, msg_len);
    sha256_hw_rfinal(&c, inner);
    sha256_hw_rinit_from(&c, k->oh, 64);
    sha256_hw_rupdate(&c, inner, 32);
    sha256_hw_rfinal(&c, out);
}

/* ── HMAC-SHA-256 (hardware HMAC wrapper) ────────────────────────────────── */

void hmac_sha256_hw(const uint8_t *key, size_t key_len,
                    const uint8_t *msg, size_t msg_len,
                    uint8_t out[32]) {
    /* Key must be ≤ 32 bytes. Longer keys should be pre-hashed by caller. */
    uint8_t key_buf[32];
    memset(key_buf, 0, 32);
    if (key_len > 32) key_len = 32;
    memcpy(key_buf, key, key_len);

    /* Load key into HMAC_KEY0..3 (256-bit = 4 × 64-bit). */
    volatile uint64_t *hkey = (volatile uint64_t *)(SHA_BASE + 0x090u);
    for (int i = 0; i < 4; i++) {
        uint64_t w;
        memcpy(&w, key_buf + i * 8, 8);
        hkey[i] = w;
    }
    REG_HMAC_CTRL = HMAC_CTRL_KEY_LOAD;
    hmac_wait();

    /* Inner hash: START loads H <- inner midstate (the SHA-256 state after the
     * ipad block, which KEY_LOAD precomputed).  START/FINAL are pure H-loads —
     * they do NOT compress — so every message block, including the final padded
     * one, must be driven with SHA_CTRL_START. */
    REG_HMAC_CTRL = HMAC_CTRL_START;
    hmac_wait();

    /* Compress the full 64-byte message blocks. */
    size_t off = 0;
    while (off + 64 <= msg_len) {
        sha_write_block(msg + off);
        REG_SHA_CTRL = SHA_CTRL_START;
        sha_wait();
        off += 64;
    }

    /* Final inner block(s): remaining bytes, 0x80, then the 64-bit big-endian
     * bit length of the inner message = ipad(64) + msg = (512 + msg_len*8) bits.
     * A tail of >= 56 bytes leaves no room for the length field, so it needs a
     * second padding block. */
    uint64_t inner_bits = 512ULL + (uint64_t)msg_len * 8ULL;
    size_t rem = msg_len - off;
    uint8_t fin[128];
    memset(fin, 0, sizeof(fin));
    memcpy(fin, msg + off, rem);
    fin[rem] = 0x80;
    size_t inblocks = (rem < 56) ? 1 : 2;
    size_t lpos = inblocks * 64 - 8;
    for (int i = 0; i < 8; i++)
        fin[lpos + 7 - i] = (uint8_t)(inner_bits >> (i * 8));
    for (size_t b = 0; b < inblocks; b++) {
        sha_write_block(fin + b * 64);
        REG_SHA_CTRL = SHA_CTRL_START;
        sha_wait();
    }

    /* H now holds the inner digest = SHA-256(ipad || msg). */
    uint8_t inner[32];
    sha_read_digest(inner);

    /* Outer hash: FINAL loads H <- outer midstate (SHA-256 state after the opad
     * block).  Then compress ONE block = inner_digest(32) || 0x80 || zeros ||
     * length, where the outer message is opad(64) + inner_digest(32) = 96 bytes
     * = 768 bits.  The previous code issued FINAL after writing the final inner
     * block but never SHA_CTRL_START'd it and never hashed this outer block, so
     * it returned the raw outer midstate instead of the HMAC. */
    REG_HMAC_CTRL = HMAC_CTRL_FINAL;
    hmac_wait();

    uint8_t ob[64];
    memset(ob, 0, 64);
    memcpy(ob, inner, 32);
    ob[32] = 0x80;
    uint64_t outer_bits = 768ULL; /* (64 opad + 32 inner) * 8 */
    for (int i = 0; i < 8; i++)
        ob[63 - i] = (uint8_t)(outer_bits >> (i * 8));
    sha_write_block(ob);
    REG_SHA_CTRL = SHA_CTRL_START;
    sha_wait();

    sha_read_digest(out);
}

/* ── HMAC-SHA-256 (software HMAC using hardware SHA-256) ────────────────── */

void hmac_sha256_sw(const uint8_t *key, size_t key_len,
                    const uint8_t *msg, size_t msg_len,
                    uint8_t out[32]) {
    uint8_t k[64], ipad[64], opad[64], inner[32];

    /* If key > 64 bytes, hash it. */
    memset(k, 0, 64);
    if (key_len > 64) {
        sha256_hw(key, key_len, k);
    } else {
        memcpy(k, key, key_len);
    }

    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36u;
        opad[i] = k[i] ^ 0x5Cu;
    }

    /* Inner hash: SHA256(ipad ‖ msg). */
    sha256_ctx_t ctx;
    sha256_hw_init(&ctx);
    sha256_hw_update(&ctx, ipad, 64);
    sha256_hw_update(&ctx, msg, msg_len);
    sha256_hw_final(&ctx, inner);

    /* Outer hash: SHA256(opad ‖ inner). */
    sha256_hw_init(&ctx);
    sha256_hw_update(&ctx, opad, 64);
    sha256_hw_update(&ctx, inner, 32);
    sha256_hw_final(&ctx, out);
}

/* ── TRNG ────────────────────────────────────────────────────────────────── */

void trng_init(void) {
    REG_TRNG_CTRL = TRNG_CTRL_ENABLE;

    /* Drain any stale FIFO entries from before init. */
    uint32_t drain = 32;
    while ((REG_TRNG_STATUS & TRNG_STATUS_READY) && drain--)
        (void)REG_TRNG_DATA;

    /* Reseed: self-clearing bit; ring oscillators refill the FIFO with
     * fresh entropy. */
    REG_TRNG_CTRL = TRNG_CTRL_ENABLE | TRNG_CTRL_RESEED;

    /* Wait for the first fresh conditioned word to appear. */
    while (!(REG_TRNG_STATUS & TRNG_STATUS_READY)) {}
}

int trng_ready(void) {
    return (REG_TRNG_STATUS & TRNG_STATUS_READY) != 0;
}

int trng_health_ok(void) {
    return (REG_TRNG_STATUS & TRNG_STATUS_HEALTH_OK) != 0;
}

void trng_read(uint8_t *buf, size_t len) {
    while (len >= 8) {
        while (!(REG_TRNG_STATUS & TRNG_STATUS_READY)) {}
        uint64_t w = REG_TRNG_DATA;
        memcpy(buf, &w, 8);
        buf += 8; len -= 8;
    }
    if (len > 0) {
        while (!(REG_TRNG_STATUS & TRNG_STATUS_READY)) {}
        uint64_t w = REG_TRNG_DATA;
        memcpy(buf, &w, len);
    }
}

/* crypto_selftest2.c — tests for the klaussCPU crypto-restore-ctr additions.
 *
 *   AES CTR mode   (AES_CAP_CTR / AES_CAP_INC32): NIST SP 800-38A F.5.1,
 *                  128-bit carry and inc32 wrap, counter readback, partial
 *                  blocks and their keystream, legacy ECB afterwards, and the
 *                  aes_ctr / AES-GCM paths that now use CTR mode
 *   SHA RESTORE    (SHA_CAP_RESTORE / SHA_CAP_ZERO): FIPS 180-4 vectors
 *                  through the save/restore context, interleaved contexts,
 *                  RESTORE|START in one write, ZERO, and HMAC from software
 *                  midstates (RFC 4231 TC1-4, a long key, interleaved keys)
 *
 * The new paths are checked against the legacy ones (hardware ECB with a
 * software counter, one-shot sha256_hw, hmac_sha256_sw), which
 * crypto_selftest covers.  Each block, and the timing, prints SKIP on a
 * bitstream (or the emulator) whose capability registers read 0.  The TIME
 * lines' numbers vary run to run.
 */

#include <stdio.h>
#include <string.h>
#include "../../crypto_hw.h"
#include "../../mmio.h"

static int pass_count, fail_count;

static void print_hex(const char *label, const uint8_t *buf, size_t len) {
    printf("  %s: ", label);
    for (size_t i = 0; i < len; i++) printf("%02x", buf[i]);
    printf("\n");
}

static int check(const char *name, const uint8_t *got, const uint8_t *want, size_t len) {
    if (memcmp(got, want, len) == 0) {
        printf("PASS  %s\n", name);
        pass_count++;
        return 1;
    }
    printf("FAIL  %s\n", name);
    print_hex("     got ", got, len);
    print_hex("    want ", want, len);
    fail_count++;
    return 0;
}

static void check_true(const char *name, int ok) {
    printf("%s  %s\n", ok ? "PASS" : "FAIL", name);
    if (ok) pass_count++; else fail_count++;
}

static void from_hex(const char *hex, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint8_t hi = hex[i*2], lo = hex[i*2+1];
        hi = (hi >= 'a') ? (hi - 'a' + 10) : (hi - '0');
        lo = (lo >= 'a') ? (lo - 'a' + 10) : (lo - '0');
        out[i] = (hi << 4) | lo;
    }
}

static uint32_t rng = 0x2545F491u;
static void fill(uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        b[i] = (uint8_t)rng;
    }
}

static void set_key(const uint8_t key[16]) {
    uint64_t lo, hi;
    memcpy(&lo, key, 8); memcpy(&hi, key + 8, 8);
    aes_hw_set_key(lo, hi);
}

#define BUF 16384
static uint8_t in_buf[BUF], out_hw[BUF], out_ref[BUF];

/* ── Reference CTR: hardware ECB + software counter ──────────────────────── */

static void ctr_inc_ref(uint8_t c[16], int inc32) {
    for (int i = 15; i >= (inc32 ? 12 : 0); i--)
        if (++c[i]) break;
}

static void ecb(const uint8_t in[16], uint8_t out[16]) {
    uint64_t a, b, x, y;
    memcpy(&a, in, 8); memcpy(&b, in + 8, 8);
    aes_hw_encrypt(a, b, &x, &y);
    memcpy(out, &x, 8); memcpy(out + 8, &y, 8);
}

/* Like aes_ctr_hw: ctr advances past every block used; ks_tail as there. */
static void ctr_ref(uint8_t ctr[16], const uint8_t *in, uint8_t *out, size_t len,
                    int inc32, uint8_t ks_tail[16]) {
    uint8_t ks[16];
    while (len > 0) {
        size_t n = len < 16 ? len : 16;
        ecb(ctr, ks);
        ctr_inc_ref(ctr, inc32);
        for (size_t i = 0; i < n; i++) out[i] = in[i] ^ ks[i];
        if (n < 16 && ks_tail) memcpy(ks_tail, ks, 16);
        in += n; out += n; len -= n;
    }
}

/* ── AES CTR mode ────────────────────────────────────────────────────────── */

static void ctr_case(const char *name, const uint8_t start[16], size_t len, int inc32) {
    uint8_t c_hw[16], c_ref[16], ks_hw[16], ks_ref[16];
    char label[96];

    memcpy(c_hw, start, 16); memcpy(c_ref, start, 16);
    memset(ks_hw, 0, 16); memset(ks_ref, 0, 16);
    fill(in_buf, len);
    aes_ctr_hw(c_hw, in_buf, out_hw, len, inc32, ks_hw);
    ctr_ref(c_ref, in_buf, out_ref, len, inc32, ks_ref);
    snprintf(label, sizeof(label), "%s: output", name);
    check(label, out_hw, out_ref, len);
    snprintf(label, sizeof(label), "%s: counter readback", name);
    check(label, c_hw, c_ref, 16);
    if (len % 16) {
        snprintf(label, sizeof(label), "%s: tail keystream", name);
        check(label, ks_hw, ks_ref, 16);
    }
}

/* AES-GCM known answers (McGrew & Viega test cases 2-4; TC4 has a partial
 * final block and 20 bytes of AAD), encrypt and decrypt. */
static void gcm_kat(const char *name, const char *k, const char *ivh,
                    const char *ah, size_t alen, const char *ph,
                    const char *ch, size_t n, const char *th) {
    uint8_t key[16], iv[12], a[20], p[64], c[64], t[16], got[64], gt[16];
    char nm[64];
    from_hex(k, key, 16); from_hex(ivh, iv, 12);
    from_hex(ah, a, alen); from_hex(ph, p, n); from_hex(ch, c, n);
    from_hex(th, t, 16);
    set_key(key);
    aes_gcm_encrypt(iv, alen ? a : NULL, alen, p, got, n, gt);
    snprintf(nm, sizeof nm, "%s encrypt ciphertext", name);
    check(nm, got, c, n);
    snprintf(nm, sizeof nm, "%s encrypt tag", name);
    check(nm, gt, t, 16);
    memset(got, 0xA5, sizeof got);
    int rc = aes_gcm_decrypt(iv, alen ? a : NULL, alen, c, got, n, t);
    snprintf(nm, sizeof nm, "%s decrypt", name);
    check_true(nm, rc == 0 && memcmp(got, p, n) == 0);
}

static void test_gcm_kats(void) {
    static const char z16[] = "00000000000000000000000000000000";
    static const char k3[] = "feffe9928665731c6d6a8f9467308308";
    static const char iv3[] = "cafebabefacedbaddecaf888";
    static const char p3[] =
        "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
        "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b391aafd255";
    static const char c3[] =
        "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e"
        "21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091473f5985";
    gcm_kat("GCM TC2", z16, "000000000000000000000000", "", 0, z16,
            "0388dace60b6a392f328c2b971b2fe78", 16,
            "ab6e47d42cec13bdf53a67b21257bddf");
    gcm_kat("GCM TC3", k3, iv3, "", 0, p3, c3, 64,
            "4d5c2af327cd64a62cf35abd2ba6fab4");
    gcm_kat("GCM TC4", k3, iv3, "feedfacedeadbeeffeedfacedeadbeefabaddad2", 20,
            p3, c3, 60, "5bc94fbc3221a5db94fae95ae7121a47");
}

static void test_aes_ctr_mode(void) {
    printf("\n=== AES CTR mode (AES_CAP=0x%x) ===\n", (unsigned)aes_hw_caps());
    if ((aes_hw_caps() & (AES_CAP_CTR | AES_CAP_INC32)) != (AES_CAP_CTR | AES_CAP_INC32)) {
        printf("SKIP  AES CTR mode not in this bitstream\n");
        return;
    }

    /* SP 800-38A F.5.1 */
    uint8_t key[16], ctr[16], pt[64], ct_exp[64], ct[64], end[16];
    from_hex("2b7e151628aed2a6abf7158809cf4f3c", key, 16);
    from_hex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", ctr, 16);
    from_hex("6bc1bee22e409f96e93d7e117393172a" "ae2d8a571e03ac9c9eb76fac45af8e51"
             "30c81c46a35ce411e5fbc1191a0a52ef" "f69f2445df4f9b17ad2b417be66c3710", pt, 64);
    from_hex("874d6191b620e3261bef6864990db6ce" "9806f66b7970fdff8617187bb9fffdff"
             "5ae4df3edbd5d35e5b4f09020db03eab" "1e031dda2fbe03d1792170a0f3009cee", ct_exp, 64);
    from_hex("f0f1f2f3f4f5f6f7f8f9fafbfcfdff03", end, 16);
    set_key(key);
    aes_ctr_hw(ctr, pt, ct, 64, 0, NULL);
    check("CTR F.5.1 4 blocks", ct, ct_exp, 64);
    check("CTR F.5.1 counter = ICB + 4", ctr, end, 16);

    uint8_t s[16];
    fill(key, 16);
    set_key(key);
    from_hex("0001020304050607ffffffffffffffff", s, 16);
    ctr_case("CTR carry into byte 7", s, 64, 0);
    from_hex("00010203ffffffffffffffffffffffff", s, 16);
    ctr_case("CTR carry across bytes 7..3", s, 64, 0);
    memset(s, 0xff, 16);
    ctr_case("CTR all-ones wraps to zero", s, 64, 0);
    fill(s, 12); s[12] = 0xff; s[13] = 0xff; s[14] = 0xff; s[15] = 0xfe;
    ctr_case("CTR inc32 wrap (byte 11 kept)", s, 64, 1);
    fill(s, 16);
    ctr_case("CTR 37 bytes", s, 37, 0);
    fill(s, 16);
    ctr_case("CTR 4096 bytes", s, 4096, 0);

    /* aes_ctr() now runs on CTR mode: the same as the reference. */
    uint64_t lo, hi;
    fill(s, 16);
    memcpy(&lo, s, 8); memcpy(&hi, s + 8, 8);
    fill(in_buf, 100);
    aes_ctr(lo, hi, in_buf, out_hw, 100);
    ctr_ref(s, in_buf, out_ref, 100, 0, NULL);
    check("aes_ctr() via CTR mode", out_hw, out_ref, 100);

    /* Legacy ECB still works after CTR mode (FIPS-197 C.1). */
    uint8_t p[16], c_exp[16], c[16];
    from_hex("000102030405060708090a0b0c0d0e0f", key, 16);
    from_hex("00112233445566778899aabbccddeeff", p, 16);
    from_hex("69c4e0d86a7b0430d8cdb78070b4c55a", c_exp, 16);
    set_key(key);
    ecb(p, c);
    check("ECB after CTR mode", c, c_exp, 16);
    check_true("AES_MODE back to 0", REG_AES_MODE == 0);

    /* GCM (CTR mode + GHASH): round trips at awkward lengths; the NIST
     * vector is in crypto_selftest. */
    static const size_t lens[] = {0, 1, 15, 16, 17, 100, 1024, 4096};
    uint8_t iv[12], aad[20], tag[16];
    int ok = 1;
    fill(key, 16); set_key(key);
    for (size_t i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
        size_t n = lens[i];
        fill(iv, 12); fill(aad, 20); fill(in_buf, n);
        aes_gcm_encrypt(iv, aad, 20, in_buf, out_hw, n, tag);
        memset(out_ref, 0, n);
        if (aes_gcm_decrypt(iv, aad, 20, out_hw, out_ref, n, tag) != 0 ||
            memcmp(out_ref, in_buf, n) != 0) ok = 0;
        tag[0] ^= 1;
        if (aes_gcm_decrypt(iv, aad, 20, out_hw, out_ref, n, tag) == 0) ok = 0;
    }
    check_true("GCM round trips and tamper checks, lengths 0..4096", ok);
    test_gcm_kats();
    check_true("AES_MODE back to 0 after GCM", REG_AES_MODE == 0);
}

/* ── SHA RESTORE ─────────────────────────────────────────────────────────── */

static void sha_r_oneshot(const uint8_t *m, size_t n, uint8_t out[32]) {
    sha256_hw_rctx_t c;
    sha256_hw_rinit(&c);
    sha256_hw_rupdate(&c, m, n);
    sha256_hw_rfinal(&c, out);
}

static void test_sha_restore(void) {
    printf("\n=== SHA-256 save/restore (SHA_CAP=0x%x) ===\n", (unsigned)sha_hw_caps());
    if ((sha_hw_caps() & (SHA_CAP_RESTORE | SHA_CAP_ZERO)) != (SHA_CAP_RESTORE | SHA_CAP_ZERO)) {
        printf("SKIP  SHA RESTORE not in this bitstream\n");
        return;
    }

    uint8_t d[32], want[32];
    sha_r_oneshot((const uint8_t *)"abc", 3, d);
    from_hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", want, 32);
    check("SHA256(\"abc\") restore path", d, want, 32);
    sha_r_oneshot((const uint8_t *)"", 0, d);
    from_hex("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", want, 32);
    check("SHA256(\"\") restore path", d, want, 32);
    const char *m448 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha_r_oneshot((const uint8_t *)m448, 56, d);
    from_hex("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", want, 32);
    check("SHA256(448-bit) restore path", d, want, 32);

    /* Chunked updates at every length 0..300 against the one-shot. */
    int ok = 1;
    fill(in_buf, 300);
    for (size_t n = 0; n <= 300; n++) {
        sha256_hw_rctx_t c;
        size_t a = n / 3, b = n / 2;
        sha256_hw_rinit(&c);
        sha256_hw_rupdate(&c, in_buf, a);
        sha256_hw_rupdate(&c, in_buf + a, b - a);
        sha256_hw_rupdate(&c, in_buf + b, n - b);
        sha256_hw_rfinal(&c, d);
        sha256_hw(in_buf, n, want);
        if (memcmp(d, want, 32) != 0) { ok = 0; printf("  mismatch at length %u\n", (unsigned)n); break; }
    }
    check_true("SHA256 chunked restore path == one-shot, lengths 0..300", ok);

    /* Two contexts and a legacy one-shot (INIT) interleaved. */
    sha256_hw_rctx_t A, B;
    uint8_t da[32], db[32], other[32], wa[32], wb[32];
    fill(in_buf, 1000);
    sha256_hw_rinit(&A); sha256_hw_rinit(&B);
    sha256_hw_rupdate(&A, in_buf, 130);
    sha256_hw(in_buf + 500, 77, other);
    sha256_hw_rupdate(&B, in_buf + 200, 64);
    sha256_hw_rupdate(&A, in_buf + 130, 370);
    sha256_hw_rupdate(&B, in_buf + 264, 236);
    sha256_hw_rfinal(&B, db);
    sha256_hw_rfinal(&A, da);
    sha256_hw(in_buf, 500, wa);
    sha256_hw(in_buf + 200, 300, wb);
    check("SHA256 interleaved context A", da, wa, 32);
    check("SHA256 interleaved context B", db, wb, 32);

    /* RESTORE|START in one write vs RESTORE then START (register level). */
    uint64_t h[4], r1[4], r2[4];
    sha256_hw_rinit(&A);
    sha256_hw_rupdate(&A, in_buf, 64);              /* state after block 1 */
    memcpy(h, A.h, sizeof(h));
    for (int pass = 0; pass < 2; pass++) {
        uint64_t *r = pass ? r2 : r1;
        for (int i = 0; i < 4; i++) REG_SHA_HIN(i) = h[i];
        for (int i = 0; i < 8; i++) { uint64_t w; memcpy(&w, in_buf + 64 + i * 8, 8); REG_SHA_BLOCK(i) = w; }
        if (pass == 0) {
            REG_SHA_CTRL = SHA_CTRL_RESTORE | SHA_CTRL_START;
        } else {
            REG_SHA_CTRL = SHA_CTRL_RESTORE;
            while (REG_SHA_STATUS & SHA_STATUS_BUSY) {}
            REG_SHA_CTRL = SHA_CTRL_START;
        }
        while ((REG_SHA_STATUS & SHA_STATUS_BUSY) || !(REG_SHA_STATUS & SHA_STATUS_DONE)) {}
        for (int i = 0; i < 4; i++) r[i] = REG_SHA_DIGEST(i);
    }
    check("RESTORE|START == RESTORE; START", (uint8_t *)r1, (uint8_t *)r2, 32);
    sha256_hw_rupdate(&A, in_buf + 64, 64);
    check("RESTORE|START == context path", (uint8_t *)r1, (uint8_t *)A.h, 32);

    /* ZERO wipes H, HIN and BLOCK; INIT works afterwards. */
    REG_SHA_CTRL = SHA_CTRL_ZERO;
    while (REG_SHA_STATUS & SHA_STATUS_BUSY) {}
    uint64_t acc = 0;
    for (int i = 0; i < 4; i++) acc |= REG_SHA_DIGEST(i) | REG_SHA_HIN(i);
    for (int i = 0; i < 8; i++) acc |= REG_SHA_BLOCK(i);
    check_true("ZERO clears DIGEST, HIN and BLOCK", acc == 0);
    sha256_hw((const uint8_t *)"abc", 3, d);
    from_hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", want, 32);
    check("SHA256(\"abc\") after ZERO", d, want, 32);

    /* HMAC from midstates: RFC 4231 TC1-4. */
    static const struct { const char *key, *data, *mac; int klen, dlen; uint8_t kfill, dfill; } tc[] = {
        { NULL, "Hi There", "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", 20, 8, 0x0b, 0 },
        { "Jefe", "what do ya want for nothing?", "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", 4, 28, 0, 0 },
        { NULL, NULL, "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe", 20, 50, 0xaa, 0xdd },
        { NULL, NULL, "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b", 25, 50, 0, 0xcd },
    };
    uint8_t key[131], data[64];
    hmac_sha256_hw_key_t k;
    for (int t = 0; t < 4; t++) {
        char label[48];
        if (tc[t].key) memcpy(key, tc[t].key, tc[t].klen);
        else if (t == 3) for (int i = 0; i < 25; i++) key[i] = (uint8_t)(i + 1);
        else memset(key, tc[t].kfill, tc[t].klen);
        if (tc[t].data) memcpy(data, tc[t].data, tc[t].dlen);
        else memset(data, tc[t].dfill, tc[t].dlen);
        from_hex(tc[t].mac, want, 32);
        hmac_sha256_hw_rkey(&k, key, tc[t].klen);
        hmac_sha256_hw_rmac(&k, data, tc[t].dlen, d);
        snprintf(label, sizeof(label), "HMAC TC%d (midstates)", t + 1);
        check(label, d, want, 32);
    }

    /* A 131-byte key (hashed first), against the software HMAC. */
    memset(key, 0xaa, 131);
    fill(in_buf, 200);
    hmac_sha256_hw_rkey(&k, key, 131);
    hmac_sha256_hw_rmac(&k, in_buf, 200, d);
    hmac_sha256_sw(key, 131, in_buf, 200, want);
    check("HMAC 131-byte key (midstates)", d, want, 32);

    /* Two keys interleaved: K2, K1, K2. */
    uint8_t k1[32], k2[32];
    hmac_sha256_hw_key_t K1, K2;
    fill(k1, 32); fill(k2, 32);
    hmac_sha256_hw_rkey(&K1, k1, 32);
    hmac_sha256_hw_rkey(&K2, k2, 32);
    ok = 1;
    for (int t = 0; t < 3; t++) {
        const uint8_t *kk = t == 1 ? k1 : k2;
        const hmac_sha256_hw_key_t *KK = t == 1 ? &K1 : &K2;
        fill(in_buf, 100 + t * 37);
        hmac_sha256_hw_rmac(KK, in_buf, 100 + t * 37, d);
        hmac_sha256_sw(kk, 32, in_buf, 100 + t * 37, want);
        if (memcmp(d, want, 32) != 0) ok = 0;
    }
    check_true("HMAC keys interleaved K2, K1, K2", ok);
}

/* ── Timing (not part of PASS/FAIL) ──────────────────────────────────────── */

static unsigned long kibps(size_t bytes, uint64_t cycles) {
    return cycles ? (unsigned long)((uint64_t)bytes * 100000000ull / cycles / 1024) : 0;
}

static void timing(void) {
    uint8_t key[16], ctr[16], iv[12], tag[16], d[32];
    uint64_t t0, t1, t2;

    printf("\n=== Timing (100 MHz) ===\n");
    if (!(aes_hw_caps() & AES_CAP_CTR) && !(sha_hw_caps() & SHA_CAP_RESTORE)) {
        /* An older bitstream, or the emulator, which has no crypto blocks
         * at all: their status polls would never finish there. */
        printf("SKIP  timing: no CTR / RESTORE in this bitstream\n");
        return;
    }
    fill(key, 16); set_key(key);
    fill(in_buf, BUF);
    if (aes_hw_caps() & AES_CAP_CTR) {
        fill(ctr, 16);
        t0 = REG_PERF_CYCLES;
        ctr_ref(ctr, in_buf, out_ref, BUF, 0, NULL);
        t1 = REG_PERF_CYCLES;
        aes_ctr_hw(ctr, in_buf, out_hw, BUF, 0, NULL);
        t2 = REG_PERF_CYCLES;
        printf("TIME  AES-128-CTR 16 KiB  legacy %lu KiB/s  ctr-mode %lu KiB/s\n",
               kibps(BUF, t1 - t0), kibps(BUF, t2 - t1));
    }
    fill(iv, 12);
    t0 = REG_PERF_CYCLES;
    aes_gcm_encrypt(iv, NULL, 0, in_buf, out_hw, BUF, tag);
    t1 = REG_PERF_CYCLES;
    printf("TIME  AES-128-GCM 16 KiB  %lu KiB/s\n", kibps(BUF, t1 - t0));

    t0 = REG_PERF_CYCLES;
    sha256_hw(in_buf, BUF, d);
    t1 = REG_PERF_CYCLES;
    printf("TIME  SHA-256 16 KiB  one-shot %lu KiB/s", kibps(BUF, t1 - t0));
    if (sha_hw_caps() & SHA_CAP_RESTORE) {
        sha256_hw_rctx_t c;
        t0 = REG_PERF_CYCLES;
        sha256_hw_rinit(&c);
        for (size_t off = 0; off < BUF; off += 1500)
            sha256_hw_rupdate(&c, in_buf + off, BUF - off < 1500 ? BUF - off : 1500);
        sha256_hw_rfinal(&c, d);
        t1 = REG_PERF_CYCLES;
        printf("  restore path (1500 B updates) %lu KiB/s", kibps(BUF, t1 - t0));
    }
    printf("\n");

    uint8_t hk[32];
    fill(hk, 32);
    t0 = REG_PERF_CYCLES;
    for (int i = 0; i < 256; i++) hmac_sha256_sw(hk, 32, in_buf, 64, d);
    t1 = REG_PERF_CYCLES;
    printf("TIME  HMAC-SHA-256 64 B  sw %lu cyc", (unsigned long)((t1 - t0) / 256));
    t0 = REG_PERF_CYCLES;
    for (int i = 0; i < 256; i++) hmac_sha256_hw(hk, 32, in_buf, 64, d);
    t1 = REG_PERF_CYCLES;
    printf("  wrapper %lu cyc", (unsigned long)((t1 - t0) / 256));
    if (sha_hw_caps() & SHA_CAP_RESTORE) {
        hmac_sha256_hw_key_t k;
        hmac_sha256_hw_rkey(&k, hk, 32);
        t0 = REG_PERF_CYCLES;
        for (int i = 0; i < 256; i++) hmac_sha256_hw_rmac(&k, in_buf, 64, d);
        t1 = REG_PERF_CYCLES;
        printf("  midstates %lu cyc", (unsigned long)((t1 - t0) / 256));
    }
    printf("\n");
}

int main(void) {
    printf("KlaussCPU crypto self-test 2 (CTR mode, SHA save/restore)\n");
    test_aes_ctr_mode();
    test_sha_restore();
    timing();
    printf("\n%d passed, %d failed\n", pass_count, fail_count);
    if (fail_count == 0)
        printf("ALL PASS\n");
    return fail_count ? 1 : 0;
}

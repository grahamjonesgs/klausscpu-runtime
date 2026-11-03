/* crypto_hw.h — KlaussCPU hardware crypto acceleration API.
 *
 * Wraps the five MMIO crypto blocks defined in mmio.h:
 *   AES-128 ECB core  (0xF00A_0000)
 *   AES-GCM mode      (0xF00A_0080)
 *   SHA-256 core      (0xF00B_0000)
 *   HMAC wrapper      (0xF00B_0080)
 *   TRNG              (0xF00C_0000)
 *
 * All functions are blocking (poll BUSY until done).
 * In a FreeRTOS environment, callers are responsible for serializing access
 * to each block with a mutex (crypto_hw_lock / crypto_hw_unlock).
 */
#ifndef CRYPTO_HW_H
#define CRYPTO_HW_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── AES-128 ECB ─────────────────────────────────────────────────────────── */

/* Load a 128-bit key and expand the round key schedule (~11 cycles). */
void aes_hw_set_key(uint64_t key_lo, uint64_t key_hi);

/* Encrypt / decrypt one 128-bit block (10 cycles after key is loaded). */
void aes_hw_encrypt(uint64_t in_lo, uint64_t in_hi,
                    uint64_t *out_lo, uint64_t *out_hi);
void aes_hw_decrypt(uint64_t in_lo, uint64_t in_hi,
                    uint64_t *out_lo, uint64_t *out_hi);

/* ── AES-128 CTR (software loop using hardware ECB) ──────────────────────── */

/* Encrypt or decrypt (CTR is symmetric) a byte buffer.
 * nonce_lo/hi: the initial 128-bit counter block (IV ‖ 0x00000001 for 96-bit IVs).
 * Key must have been loaded with aes_hw_set_key() before calling.
 * Handles non-block-multiple lengths via keystream tail. */
void aes_ctr(uint64_t nonce_lo, uint64_t nonce_hi,
             const uint8_t *in, uint8_t *out, size_t len);

/* Convenience: set key then run CTR. */
void aes_ctr_full(uint64_t key_lo, uint64_t key_hi,
                  uint64_t nonce_lo, uint64_t nonce_hi,
                  const uint8_t *in, uint8_t *out, size_t len);

/* ── AES-GCM ─────────────────────────────────────────────────────────────── */

/* One-shot GCM encrypt + authenticate.
 * iv: 12-byte (96-bit) IV — the hardware appends 0x00000001 to form J0.
 * aad / aad_len: additional authenticated data (may be NULL / 0).
 * pt: plaintext in;  ct: ciphertext out (same length as pt).
 * tag_out: 16-byte authentication tag.
 * Key must have been loaded with aes_hw_set_key(). */
void aes_gcm_encrypt(const uint8_t *iv,
                     const uint8_t *aad, size_t aad_len,
                     const uint8_t *pt, uint8_t *ct, size_t len,
                     uint8_t tag_out[16]);

/* One-shot GCM decrypt + verify.
 * Returns 0 if tag matches, -1 if authentication failed (output undefined). */
int  aes_gcm_decrypt(const uint8_t *iv,
                     const uint8_t *aad, size_t aad_len,
                     const uint8_t *ct, uint8_t *pt, size_t len,
                     const uint8_t tag_in[16]);

/* ── SHA-256 ─────────────────────────────────────────────────────────────── */

/* One-shot SHA-256. out must point to a 32-byte buffer. */
void sha256_hw(const uint8_t *msg, size_t len, uint8_t out[32]);

/* Streaming SHA-256 (for messages that arrive in chunks). */
typedef struct {
    uint64_t total_bytes; /* bytes processed so far */
    uint8_t  buf[64];     /* partial block buffer   */
    uint32_t buf_len;     /* bytes in buf           */
    int      initialized; /* non-zero after sha256_hw_init */
} sha256_ctx_t;

void sha256_hw_init(sha256_ctx_t *ctx);
void sha256_hw_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len);
void sha256_hw_final(sha256_ctx_t *ctx, uint8_t out[32]);

/* ── HMAC-SHA-256 ────────────────────────────────────────────────────────── */

/* Hardware HMAC (uses HMAC wrapper block; key must be ≤ 32 bytes).
 * For keys > 32 bytes, SHA-256-hash the key first. */
void hmac_sha256_hw(const uint8_t *key, size_t key_len,
                    const uint8_t *msg, size_t msg_len,
                    uint8_t out[32]);

/* Software HMAC using hardware SHA-256 (fallback; supports any key length). */
void hmac_sha256_sw(const uint8_t *key, size_t key_len,
                    const uint8_t *msg, size_t msg_len,
                    uint8_t out[32]);

/* ── Capabilities ────────────────────────────────────────────────────────── */

/* Feature bits of the loaded bitstream (AES_CAP_* / SHA_CAP_* in mmio.h).
 * Older bitstreams and the emulator read 0.  The functions above use the
 * fast paths below on their own when the bits are set. */
uint32_t aes_hw_caps(void);
uint32_t sha_hw_caps(void);

/* ── AES-128 CTR in hardware (needs AES_CAP_CTR) ─────────────────────────── */

/* CTR-encrypt/decrypt len bytes with the loaded key.  ctr is the 16-byte
 * counter block and is advanced past every block used; inc32 selects GCM's
 * 32-bit increment instead of the full 128-bit one.  For a final partial
 * block, ks_tail (if non-NULL) receives that block's whole keystream, so a
 * caller can use its remaining bytes on the next call. */
void aes_ctr_hw(uint8_t ctr[16], const uint8_t *in, uint8_t *out, size_t len,
                int inc32, uint8_t ks_tail[16]);

/* ── SHA-256 with state save/restore (needs SHA_CAP_RESTORE) ─────────────── */

/* A context that can be suspended between calls: the hardware holds it only
 * inside sha256_hw_rupdate/rfinal, so any number may be in flight (callers
 * still serialize access to the block).  h is in SHA_DIGEST's packing. */
typedef struct {
    uint64_t h[4];
    uint64_t total_bytes;
    uint8_t  buf[64];
    uint32_t buf_len;
} sha256_hw_rctx_t;

void sha256_hw_rinit(sha256_hw_rctx_t *ctx);
/* Start from a saved state h that has already hashed prefix_bytes (a
 * multiple of 64), e.g. an HMAC midstate. */
void sha256_hw_rinit_from(sha256_hw_rctx_t *ctx, const uint64_t h[4],
                          uint64_t prefix_bytes);
void sha256_hw_rupdate(sha256_hw_rctx_t *ctx, const uint8_t *data, size_t len);
void sha256_hw_rfinal(sha256_hw_rctx_t *ctx, uint8_t out[32]);

/* HMAC-SHA-256 from software midstates: any number of keys, any key length. */
typedef struct {
    uint64_t ih[4];   /* state after the ipad block */
    uint64_t oh[4];   /* state after the opad block */
} hmac_sha256_hw_key_t;

void hmac_sha256_hw_rkey(hmac_sha256_hw_key_t *k, const uint8_t *key, size_t key_len);
void hmac_sha256_hw_rmac(const hmac_sha256_hw_key_t *k, const uint8_t *msg,
                         size_t msg_len, uint8_t out[32]);

/* ── TRNG ────────────────────────────────────────────────────────────────── */

/* Enable the TRNG and wait until the first conditioned output is ready.
 * Call once at startup before any trng_read(). */
void trng_init(void);

/* Returns non-zero if ≥1 word is buffered in the TRNG FIFO. */
int trng_ready(void);

/* Returns non-zero if the NIST SP 800-90B continuous health tests are passing.
 * Treat 0 as fatal — refuse key operations and reboot. */
int trng_health_ok(void);

/* Fill buf with len random bytes from the TRNG.
 * Blocks until enough conditioned words are available.
 * Does NOT check health; call trng_health_ok() separately. */
void trng_read(uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* CRYPTO_HW_H */

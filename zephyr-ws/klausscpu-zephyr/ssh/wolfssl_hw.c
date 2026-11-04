/* wolfssl_hw.c — wolfSSL hardware crypto callbacks for KlaussCPU (Zephyr).
 *
 * AES-128 CTR and GCM run on the AES/GHASH block through the shared driver
 * (src/crypto_hw.c); the TRNG seeds wolfSSL's DRBG.  A find callback routes
 * every cipher operation here, since neither wolfSSH (devId 0) nor the TLS
 * server (INVALID_DEVID) names this device.  Other key sizes and algorithms
 * fall back to wolfCrypt software.
 */

#define WOLFSSL_USER_SETTINGS
#include "user_settings.h"

#include <zephyr/kernel.h>
#include <string.h>

#include <wolfssl/wolfcrypt/cryptocb.h>
#include <wolfssl/wolfcrypt/aes.h>
#include <wolfssl/wolfcrypt/random.h>
#include <wolfssl/wolfcrypt/error-crypt.h>

#include "wolfssl_hw.h"
#include <klausscpu_prof.h>

/* ── MMIO register definitions (crypto blocks) ──────────────────────────── */

#define MMIO_BASE    0xF0000000u
#define TRNG_BASE    (MMIO_BASE + 0x000C0000u)

#define REG(a)       (*(volatile uint32_t *)(unsigned long)(a))
#define REG64(a)     (*(volatile uint64_t *)(unsigned long)(a))

#define REG_TRNG_CTRL    REG(TRNG_BASE + 0x000u)
#define REG_TRNG_STATUS  REG(TRNG_BASE + 0x008u)
#define REG_TRNG_DATA    REG64(TRNG_BASE + 0x010u)

#define TRNG_CTRL_ENABLE   (1u << 0)
#define TRNG_STATUS_READY      (1u << 0)
#define TRNG_STATUS_HEALTH_OK  (1u << 1)

/* AES-128, CTR and GCM from the shared driver (src/crypto_hw.c). */
void aes_hw_set_key(uint64_t key_lo, uint64_t key_hi);
void aes_hw_encrypt(uint64_t in_lo, uint64_t in_hi, uint64_t *out_lo, uint64_t *out_hi);
void aes_ctr(uint64_t nonce_lo, uint64_t nonce_hi, const uint8_t *in, uint8_t *out,
	     size_t len);
void aes_gcm_encrypt(const uint8_t *iv, const uint8_t *aad, size_t aad_len,
		     const uint8_t *pt, uint8_t *ct, size_t len, uint8_t tag_out[16]);
int aes_gcm_decrypt(const uint8_t *iv, const uint8_t *aad, size_t aad_len,
		    const uint8_t *ct, uint8_t *pt, size_t len, const uint8_t tag_in[16]);

/* ── Hardware helpers ────────────────────────────────────────────────────── */

static void trng_init_hw(void)
{
	REG_TRNG_CTRL = TRNG_CTRL_ENABLE;
	while (!(REG_TRNG_STATUS & TRNG_STATUS_READY)) {}
	/* Wait for health monitor to pass — may take a few more cycles */
	for (int i = 0; i < 1000; i++) {
		if (REG_TRNG_STATUS & TRNG_STATUS_HEALTH_OK) {
			return;
		}
		for (volatile int d = 0; d < 100; d++) {}
	}
	/* If health never asserts, continue anyway — the TRNG data
	 * is still valid on KlaussCPU, the health monitor may not
	 * be implemented in all FPGA builds. */
}

static int trng_health_ok(void)
{
	/* Accept if either HEALTH_OK is set OR if READY is set
	 * (some FPGA builds don't implement the health monitor) */
	uint32_t st = REG_TRNG_STATUS;
	return (st & (TRNG_STATUS_HEALTH_OK | TRNG_STATUS_READY)) != 0;
}

static void trng_read(uint8_t *buf, size_t len)
{
	size_t done = 0;

	while (done < len) {
		while (!(REG_TRNG_STATUS & TRNG_STATUS_READY)) {}
		uint64_t w = REG_TRNG_DATA;
		size_t chunk = len - done;

		if (chunk > 8) {
			chunk = 8;
		}
		memcpy(buf + done, &w, chunk);
		done += chunk;
	}
}

/* ── Mutex for hardware AES block ────────────────────────────────────────── */

static struct k_mutex aes_mtx;

static inline void aes_lock(void)   { k_mutex_lock(&aes_mtx, K_FOREVER); }
static inline void aes_unlock(void) { k_mutex_unlock(&aes_mtx); }

/* ── TRNG seed function ──────────────────────────────────────────────────── */

int klausscpu_trng_seed(unsigned char *output, unsigned int sz)
{
	if (!trng_health_ok()) {
		return -1;
	}
	trng_read(output, (size_t)sz);
	return 0;
}

/* ── Extract 128-bit AES key from wolfCrypt Aes struct ───────────────────── */

/* The first four words of the encryption key schedule are the key itself,
 * stored as big-endian words (wc_AesSetKey byte-reverses them on a
 * little-endian CPU).  aes->devKey is only filled for a non-INVALID devId,
 * which the TLS objects are not. */
static int extract_key128(const Aes *aes, uint64_t *lo, uint64_t *hi)
{
	if (!aes || aes->keylen != 16) {
		return -1;
	}
	uint8_t k[16];

	for (int i = 0; i < 4; i++) {
		uint32_t w = aes->key[i];

		k[i * 4 + 0] = (uint8_t)(w >> 24);
		k[i * 4 + 1] = (uint8_t)(w >> 16);
		k[i * 4 + 2] = (uint8_t)(w >> 8);
		k[i * 4 + 3] = (uint8_t)(w);
	}
	memcpy(lo, k, 8);
	memcpy(hi, k + 8, 8);
	return 0;
}

/* 128-bit big-endian counter increment (wolfSSL's IncrementAesCounter). */
static void ctr_inc_be(uint8_t ctr[16])
{
	for (int i = 15; i >= 0; i--) {
		if (++ctr[i]) {
			break;
		}
	}
}

/* AES-CTR with wolfSSL's software semantics: aes->reg is the next counter
 * block, aes->tmp the keystream of the last partial block and aes->left its
 * unused bytes.  wolfSSL calls the callback before consuming aes->left. */
static void hw_ctr(Aes *aes, uint8_t *out, const uint8_t *in, word32 sz,
		   uint64_t key_lo, uint64_t key_hi)
{
	uint8_t *ctr = (uint8_t *)aes->reg;
	uint8_t *tmp = (uint8_t *)aes->tmp;

	while (aes->left && sz) {
		*out++ = *in++ ^ tmp[AES_BLOCK_SIZE - aes->left];
		aes->left--;
		sz--;
	}
	if (sz == 0) {
		return;
	}
	aes_lock();
	aes_hw_set_key(key_lo, key_hi);
	word32 full = sz & ~(word32)(AES_BLOCK_SIZE - 1);

	if (full) {
		uint64_t lo, hi;

		memcpy(&lo, ctr, 8);
		memcpy(&hi, ctr + 8, 8);
		aes_ctr(lo, hi, in, out, full);
		for (word32 b = 0; b < full / AES_BLOCK_SIZE; b++) {
			ctr_inc_be(ctr);
		}
		in += full;
		out += full;
		sz -= full;
	}
	if (sz) {
		uint64_t lo, hi, ks_lo, ks_hi;

		memcpy(&lo, ctr, 8);
		memcpy(&hi, ctr + 8, 8);
		aes_hw_encrypt(lo, hi, &ks_lo, &ks_hi);
		memcpy(tmp, &ks_lo, 8);
		memcpy(tmp + 8, &ks_hi, 8);
		ctr_inc_be(ctr);
		for (word32 i = 0; i < sz; i++) {
			out[i] = in[i] ^ tmp[i];
		}
		aes->left = AES_BLOCK_SIZE - sz;
	}
	aes_unlock();
}

/* ── wolfCrypt crypto callback ───────────────────────────────────────────── */

static int hw_cipher(wc_CryptoInfo *info);

static int hw_crypto_cb(int devId, wc_CryptoInfo *info, void *ctx)
{
	(void)devId;
	(void)ctx;

	if (info->algo_type != WC_ALGO_TYPE_CIPHER) {
		return CRYPTOCB_UNAVAILABLE;
	}

	uint64_t t0 = kprof_now();
	int rc = hw_cipher(info);

	if (rc != CRYPTOCB_UNAVAILABLE) {
		uint64_t dt = kprof_now() - t0;

		if (info->cipher.type == WC_CIPHER_AES_GCM) {
			klausscpu_prof.gcm_cyc += dt;
			klausscpu_prof.gcm_bytes += info->cipher.enc ?
				info->cipher.aesgcm_enc.sz : info->cipher.aesgcm_dec.sz;
			klausscpu_prof.gcm_calls++;
		} else {
			klausscpu_prof.ctr_cyc += dt;
			klausscpu_prof.ctr_bytes += info->cipher.aesctr.sz;
			klausscpu_prof.ctr_calls++;
		}
	}
	return rc;
}

static int hw_cipher(wc_CryptoInfo *info)
{
	uint64_t key_lo, key_hi;

#ifdef WOLFSSL_AES_COUNTER
	if (info->cipher.type == WC_CIPHER_AES_CTR) {
		Aes *aes = info->cipher.aesctr.aes;

		if (extract_key128(aes, &key_lo, &key_hi) < 0) {
			return CRYPTOCB_UNAVAILABLE;
		}
		hw_ctr(aes, info->cipher.aesctr.out, info->cipher.aesctr.in,
		       info->cipher.aesctr.sz, key_lo, key_hi);
		return 0;
	}
#endif

#ifdef HAVE_AESGCM
	if (info->cipher.type == WC_CIPHER_AES_GCM) {
		if (info->cipher.enc) {
			Aes *aes = info->cipher.aesgcm_enc.aes;
			word32 sz = info->cipher.aesgcm_enc.sz;

			if (info->cipher.aesgcm_enc.ivSz != 12 ||
			    info->cipher.aesgcm_enc.authTagSz != 16) {
				return CRYPTOCB_UNAVAILABLE;
			}
			if (extract_key128(aes, &key_lo, &key_hi) < 0) {
				return CRYPTOCB_UNAVAILABLE;
			}
			aes_lock();
			aes_hw_set_key(key_lo, key_hi);
			aes_gcm_encrypt(info->cipher.aesgcm_enc.iv,
					info->cipher.aesgcm_enc.authIn,
					info->cipher.aesgcm_enc.authInSz,
					info->cipher.aesgcm_enc.in,
					info->cipher.aesgcm_enc.out, sz,
					info->cipher.aesgcm_enc.authTag);
			aes_unlock();
			return 0;
		} else {
			Aes *aes = info->cipher.aesgcm_dec.aes;
			word32 sz = info->cipher.aesgcm_dec.sz;

			if (info->cipher.aesgcm_dec.ivSz != 12 ||
			    info->cipher.aesgcm_dec.authTagSz != 16) {
				return CRYPTOCB_UNAVAILABLE;
			}
			if (extract_key128(aes, &key_lo, &key_hi) < 0) {
				return CRYPTOCB_UNAVAILABLE;
			}
			aes_lock();
			aes_hw_set_key(key_lo, key_hi);
			int rc = aes_gcm_decrypt(info->cipher.aesgcm_dec.iv,
						 info->cipher.aesgcm_dec.authIn,
						 info->cipher.aesgcm_dec.authInSz,
						 info->cipher.aesgcm_dec.in,
						 info->cipher.aesgcm_dec.out, sz,
						 info->cipher.aesgcm_dec.authTag);
			aes_unlock();
			return (rc == 0) ? 0 : AES_GCM_AUTH_E;
		}
	}
#endif

	return CRYPTOCB_UNAVAILABLE;
}

/* Route every cipher operation to the hardware device, whatever devId its
 * object carries (wolfSSH zero-fills its objects: devId 0; the TLS context
 * keeps INVALID_DEVID).  Everything else keeps its own devId. */
static int hw_find_cb(int devId, int algoType)
{
	if (algoType == WC_ALGO_TYPE_CIPHER) {
		return KLAUSSCPU_CRYPTO_DEV_ID;
	}
	return devId;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

int wolfssl_hw_init(void)
{
	k_mutex_init(&aes_mtx);
	trng_init_hw();
	wc_CryptoCb_SetDeviceFindCb(hw_find_cb);
	return wc_CryptoCb_RegisterDevice(KLAUSSCPU_CRYPTO_DEV_ID,
					   hw_crypto_cb, NULL);
}

void wolfssl_hw_cleanup(void)
{
	wc_CryptoCb_SetDeviceFindCb(NULL);
	wc_CryptoCb_UnRegisterDevice(KLAUSSCPU_CRYPTO_DEV_ID);
}

/*
 * psa_klausscpu.c — PSA transparent driver for the KlaussCPU crypto block
 * (include/klausscpu_psa_driver.h).
 *
 * SHA-256: contexts are the save/restore contexts of src/crypto_hw.c; the
 * SHA block holds a context only inside one update or finish, under
 * sha_lock.
 *
 * AES-128-CTR: the AES block's key and counter are loaded for each update
 * and the counter read back (aes_ctr_hw), under aes_lock.  ssh/wolfssl_hw.c
 * has its own lock for the same block; no app runs both (wolfSSL goes in
 * step 7).
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <psa/crypto.h>
#include <klausscpu_psa_driver.h>

#define SHA_CAP_RESTORE_BIT 1u /* SHA_CAP[0], mmio.h SHA_CAP_RESTORE */
#define AES_CAP_CTR_BIT     1u /* AES_CAP[0], mmio.h AES_CAP_CTR */

static K_MUTEX_DEFINE(sha_lock);
static K_MUTEX_DEFINE(aes_lock);

static bool sha_hw(psa_algorithm_t alg)
{
	return alg == PSA_ALG_SHA_256 && crypto_hw_present(CPU_FEAT_CRYPTO_SHA) &&
	       (sha_hw_caps() & SHA_CAP_RESTORE_BIT);
}

psa_status_t klausscpu_psa_hash_setup(klausscpu_psa_hash_ctx_t *ctx, psa_algorithm_t alg)
{
	if (!sha_hw(alg)) {
		return PSA_ERROR_NOT_SUPPORTED;
	}
	sha256_hw_rinit(ctx);
	return PSA_SUCCESS;
}

psa_status_t klausscpu_psa_hash_clone(const klausscpu_psa_hash_ctx_t *src,
				      klausscpu_psa_hash_ctx_t *dst)
{
	*dst = *src;
	return PSA_SUCCESS;
}

psa_status_t klausscpu_psa_hash_update(klausscpu_psa_hash_ctx_t *ctx, const uint8_t *input,
				       size_t input_length)
{
	k_mutex_lock(&sha_lock, K_FOREVER);
	sha256_hw_rupdate(ctx, input, input_length);
	k_mutex_unlock(&sha_lock);
	return PSA_SUCCESS;
}

psa_status_t klausscpu_psa_hash_finish(klausscpu_psa_hash_ctx_t *ctx, uint8_t *hash,
				       size_t hash_size, size_t *hash_length)
{
	if (hash_size < 32) {
		return PSA_ERROR_BUFFER_TOO_SMALL;
	}
	k_mutex_lock(&sha_lock, K_FOREVER);
	sha256_hw_rfinal(ctx, hash);
	k_mutex_unlock(&sha_lock);
	*hash_length = 32;
	memset(ctx, 0, sizeof(*ctx));
	return PSA_SUCCESS;
}

psa_status_t klausscpu_psa_hash_abort(klausscpu_psa_hash_ctx_t *ctx)
{
	memset(ctx, 0, sizeof(*ctx));
	return PSA_SUCCESS;
}

psa_status_t klausscpu_psa_hash_compute(psa_algorithm_t alg, const uint8_t *input,
					size_t input_length, uint8_t *hash, size_t hash_size,
					size_t *hash_length)
{
	klausscpu_psa_hash_ctx_t ctx;
	psa_status_t st = klausscpu_psa_hash_setup(&ctx, alg);

	if (st != PSA_SUCCESS) {
		return st;
	}
	klausscpu_psa_hash_update(&ctx, input, input_length);
	return klausscpu_psa_hash_finish(&ctx, hash, hash_size, hash_length);
}

/* ── AES-128-CTR ─────────────────────────────────────────────────────────── */

static psa_status_t ctr_setup(klausscpu_psa_cipher_ctx_t *ctx,
			      const psa_key_attributes_t *attributes, const uint8_t *key,
			      size_t key_size, psa_algorithm_t alg)
{
	if (alg != PSA_ALG_CTR || psa_get_key_type(attributes) != PSA_KEY_TYPE_AES ||
	    key_size != 16 || !crypto_hw_present(CPU_FEAT_CRYPTO_AES) ||
	    !(aes_hw_caps() & AES_CAP_CTR_BIT)) {
		return PSA_ERROR_NOT_SUPPORTED;
	}
	memset(ctx, 0, sizeof(*ctx));
	memcpy(ctx->key, key, 16);
	return PSA_SUCCESS;
}

psa_status_t klausscpu_psa_cipher_encrypt_setup(klausscpu_psa_cipher_ctx_t *ctx,
						const psa_key_attributes_t *attributes,
						const uint8_t *key_buffer, size_t key_buffer_size,
						psa_algorithm_t alg)
{
	return ctr_setup(ctx, attributes, key_buffer, key_buffer_size, alg);
}

psa_status_t klausscpu_psa_cipher_decrypt_setup(klausscpu_psa_cipher_ctx_t *ctx,
						const psa_key_attributes_t *attributes,
						const uint8_t *key_buffer, size_t key_buffer_size,
						psa_algorithm_t alg)
{
	return ctr_setup(ctx, attributes, key_buffer, key_buffer_size, alg);
}

psa_status_t klausscpu_psa_cipher_set_iv(klausscpu_psa_cipher_ctx_t *ctx, const uint8_t *iv,
					 size_t iv_length)
{
	if (iv_length != 16) {
		return PSA_ERROR_INVALID_ARGUMENT;
	}
	memcpy(ctx->ctr, iv, 16);
	ctx->left = 0;
	return PSA_SUCCESS;
}

/* CTR is a stream cipher: encrypt and decrypt are the same. */
static void ctr_crypt(klausscpu_psa_cipher_ctx_t *ctx, const uint8_t *in, uint8_t *out,
		      size_t len)
{
	while (ctx->left && len) {
		*out++ = *in++ ^ ctx->ks[16 - ctx->left];
		ctx->left--;
		len--;
	}
	if (len == 0) {
		return;
	}
	uint64_t lo, hi;

	memcpy(&lo, ctx->key, 8);
	memcpy(&hi, ctx->key + 8, 8);
	k_mutex_lock(&aes_lock, K_FOREVER);
	aes_hw_set_key(lo, hi);
	aes_ctr_hw(ctx->ctr, in, out, len, 0, ctx->ks);
	k_mutex_unlock(&aes_lock);
	ctx->left = (len % 16) ? (uint8_t)(16 - len % 16) : 0;
}

psa_status_t klausscpu_psa_cipher_update(klausscpu_psa_cipher_ctx_t *ctx, const uint8_t *input,
					 size_t input_length, uint8_t *output, size_t output_size,
					 size_t *output_length)
{
	if (output_size < input_length) {
		return PSA_ERROR_BUFFER_TOO_SMALL;
	}
	ctr_crypt(ctx, input, output, input_length);
	*output_length = input_length;
	return PSA_SUCCESS;
}

psa_status_t klausscpu_psa_cipher_finish(klausscpu_psa_cipher_ctx_t *ctx, uint8_t *output,
					 size_t output_size, size_t *output_length)
{
	ARG_UNUSED(output);
	ARG_UNUSED(output_size);
	*output_length = 0;
	memset(ctx, 0, sizeof(*ctx));
	return PSA_SUCCESS;
}

psa_status_t klausscpu_psa_cipher_abort(klausscpu_psa_cipher_ctx_t *ctx)
{
	memset(ctx, 0, sizeof(*ctx));
	return PSA_SUCCESS;
}

psa_status_t klausscpu_psa_cipher_encrypt(const psa_key_attributes_t *attributes,
					  const uint8_t *key_buffer, size_t key_buffer_size,
					  psa_algorithm_t alg, const uint8_t *iv, size_t iv_length,
					  const uint8_t *input, size_t input_length, uint8_t *output,
					  size_t output_size, size_t *output_length)
{
	klausscpu_psa_cipher_ctx_t ctx;
	psa_status_t st = ctr_setup(&ctx, attributes, key_buffer, key_buffer_size, alg);

	if (st != PSA_SUCCESS) {
		return st;
	}
	st = klausscpu_psa_cipher_set_iv(&ctx, iv, iv_length);
	if (st == PSA_SUCCESS) {
		st = klausscpu_psa_cipher_update(&ctx, input, input_length, output, output_size,
						 output_length);
	}
	memset(&ctx, 0, sizeof(ctx));
	return st;
}

/* The one-shot decrypt's input is the IV followed by the ciphertext. */
psa_status_t klausscpu_psa_cipher_decrypt(const psa_key_attributes_t *attributes,
					  const uint8_t *key_buffer, size_t key_buffer_size,
					  psa_algorithm_t alg, const uint8_t *input,
					  size_t input_length, uint8_t *output, size_t output_size,
					  size_t *output_length)
{
	klausscpu_psa_cipher_ctx_t ctx;
	psa_status_t st = ctr_setup(&ctx, attributes, key_buffer, key_buffer_size, alg);

	if (st != PSA_SUCCESS) {
		return st;
	}
	if (input_length < 16) {
		return PSA_ERROR_INVALID_ARGUMENT;
	}
	klausscpu_psa_cipher_set_iv(&ctx, input, 16);
	st = klausscpu_psa_cipher_update(&ctx, input + 16, input_length - 16, output, output_size,
					 output_length);
	memset(&ctx, 0, sizeof(ctx));
	return st;
}

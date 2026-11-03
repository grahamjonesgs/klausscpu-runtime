/*
 * psa_klausscpu.c — PSA transparent driver for the KlaussCPU crypto block
 * (include/klausscpu_psa_driver.h).
 *
 * SHA-256: contexts are the save/restore contexts of src/crypto_hw.c; the
 * SHA block holds a context only inside one update or finish, under
 * sha_lock.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <klausscpu_psa_driver.h>

#define SHA_CAP_RESTORE_BIT 1u /* SHA_CAP[0], mmio.h SHA_CAP_RESTORE */

static K_MUTEX_DEFINE(sha_lock);

static bool sha_hw(psa_algorithm_t alg)
{
	return alg == PSA_ALG_SHA_256 && (sha_hw_caps() & SHA_CAP_RESTORE_BIT);
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

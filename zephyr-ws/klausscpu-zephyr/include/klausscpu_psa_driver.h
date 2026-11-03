/* klausscpu_psa_driver.h — the KlaussCPU crypto block as a PSA transparent
 * accelerator driver for TF-PSA-Crypto (drivers/crypto/psa_klausscpu.c).
 *
 * zephyr-patches/tf-psa-crypto-klausscpu.patch includes this from the
 * dispatch layer (psa_crypto_driver_wrappers.h) and the driver-context
 * unions when PSA_CRYPTO_DRIVER_KLAUSSCPU is set, which
 * klausscpu-psa-driver-config.h does.
 *
 * SHA-256 runs on the SHA block with state save/restore (klaussCPU 27ff737,
 * SHA_CAP_RESTORE), so any number of hash operations can be in flight; the
 * built-in HMAC uses it too.  Without RESTORE, or for other algorithms, the
 * entry points return PSA_ERROR_NOT_SUPPORTED and mbedTLS's software
 * implementation takes over.
 */
#ifndef KLAUSSCPU_PSA_DRIVER_H
#define KLAUSSCPU_PSA_DRIVER_H

#include <psa/crypto_driver_common.h>

/* sha256_hw_rctx_t from the runtime's shared crypto driver (crypto_hw.h at
 * the root of klausscpu-runtime). */
#include "../../../crypto_hw.h"

typedef sha256_hw_rctx_t klausscpu_psa_hash_ctx_t;

psa_status_t klausscpu_psa_hash_compute(psa_algorithm_t alg, const uint8_t *input,
					size_t input_length, uint8_t *hash, size_t hash_size,
					size_t *hash_length);
psa_status_t klausscpu_psa_hash_setup(klausscpu_psa_hash_ctx_t *ctx, psa_algorithm_t alg);
psa_status_t klausscpu_psa_hash_clone(const klausscpu_psa_hash_ctx_t *src,
				      klausscpu_psa_hash_ctx_t *dst);
psa_status_t klausscpu_psa_hash_update(klausscpu_psa_hash_ctx_t *ctx, const uint8_t *input,
				       size_t input_length);
psa_status_t klausscpu_psa_hash_finish(klausscpu_psa_hash_ctx_t *ctx, uint8_t *hash,
				       size_t hash_size, size_t *hash_length);
psa_status_t klausscpu_psa_hash_abort(klausscpu_psa_hash_ctx_t *ctx);

#endif /* KLAUSSCPU_PSA_DRIVER_H */

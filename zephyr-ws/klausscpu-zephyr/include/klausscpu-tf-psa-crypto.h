/* klausscpu-tf-psa-crypto.h — TF-PSA-Crypto settings for KlaussCPU, included
 * after Zephyr's config-tf-psa-crypto.h (TF_PSA_CRYPTO_USER_CONFIG_FILE, a
 * board default when MBEDTLS is on).
 *
 * MBEDTLS_ECDH_VARIANT_EVEREST_ENABLED: X25519 (SSH's curve25519-sha256 and
 * TLS 1.3's x25519) through the HACL* Curve25519 code in drivers/everest,
 * which Zephyr already builds, instead of the generic ECP module.
 */
#define MBEDTLS_ECDH_VARIANT_EVEREST_ENABLED

/* klausscpu-psa-driver-config.h — TF-PSA-Crypto driver configuration
 * (TF_PSA_CRYPTO_DRIVER_CONFIG_FILE, set in the board's Kconfig.defconfig
 * when PSA_CRYPTO_DRIVER_KLAUSSCPU is on).  Turns on the KlaussCPU hooks in
 * the dispatch layer (zephyr-patches/tf-psa-crypto-klausscpu.patch).  The
 * software implementations stay compiled in as the fallback. */
#define PSA_CRYPTO_DRIVER_KLAUSSCPU
#ifndef PSA_CRYPTO_DRIVER_PRESENT
#define PSA_CRYPTO_DRIVER_PRESENT
#endif
#ifndef PSA_CRYPTO_ACCELERATOR_DRIVER_PRESENT
#define PSA_CRYPTO_ACCELERATOR_DRIVER_PRESENT
#endif

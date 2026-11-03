cmake_minimum_required(VERSION 3.20)

# Make include/zephyr/arch/klausscpu/arch.h and kernel_arch_*.h findable.
zephyr_include_directories(include)
zephyr_include_directories(arch/klausscpu/include)
# Zephyr's arch-common headers (gen_offset.h, sw_isr_common.h).  Zephyr adds
# ${ARCH_DIR}/common/include, which for an out-of-tree arch is this module's
# arch/ directory, not ZEPHYR_BASE/arch.
zephyr_include_directories(${ZEPHYR_BASE}/arch/common/include)

# Zephyr adds arch/klausscpu/ and soc/klausscpu/nexys_a7/ itself (hardware
# model v2, from the roots in zephyr/module.yml).  Only add drivers here.
add_subdirectory(drivers/serial)
add_subdirectory(drivers/timer)
add_subdirectory_ifdef(CONFIG_NET_L2_ETHERNET drivers/ethernet)
add_subdirectory_ifdef(CONFIG_DISK_ACCESS drivers/disk)
add_subdirectory_ifdef(CONFIG_ENTROPY_KLAUSSCPU_TRNG drivers/entropy)
add_subdirectory_ifdef(CONFIG_PSA_CRYPTO_DRIVER_KLAUSSCPU drivers/crypto)

# The AES/GHASH/SHA/TRNG driver shared with the bare-metal programs, for the
# wolfSSL glue (ssh/wolfssl_hw.c) and the PSA driver (drivers/crypto).
if(CONFIG_KLAUSSCPU_SSH_SERVER OR CONFIG_PSA_CRYPTO_DRIVER_KLAUSSCPU)
  zephyr_library_named(klausscpu_crypto_hw)
  zephyr_library_sources(${CMAKE_CURRENT_SOURCE_DIR}/../../src/crypto_hw.c)
endif()
add_subdirectory(ssh)
add_subdirectory_ifdef(CONFIG_KLAUSSCPU_FRAMEBUFFER vnc)

# KlaussCPU glue for MicroPython (MICROPYTHON_PLAN.md), for two builds:
#   - standalone: the application is MicroPython's ports/zephyr (which
#     defines CONFIG_MICROPY_CONFIGFILE); console on the UART.
#   - embedded: the `python` command in ssh_shell (CONFIG_KLAUSSCPU_PYTHON),
#     built from micropython/ssh.
# MicroPython itself is the west project modules/lib/micropython.
set(MICROPY_DIR ${ZEPHYR_BASE}/../modules/lib/micropython CACHE PATH
    "MicroPython source tree")
if(NOT EXISTS ${MICROPY_DIR}/py/py.cmake)
  message(FATAL_ERROR "MicroPython not found at ${MICROPY_DIR} (west update micropython)")
endif()

if(DEFINED CONFIG_MICROPY_CONFIGFILE)
  # mpconfigport_klausscpu.h, found through MP_CONFIGFILE=<...>.
  zephyr_include_directories(.)

  # Polled console input: the KlaussCPU UART has no RX interrupt, so wrap the
  # port's interrupt-fed zephyr_getchar* (src/zephyr_getchar.c) with a
  # polling thread (console_poll.c).
  zephyr_library_named(klausscpu_micropython)
  zephyr_library_sources(console_poll.c)
  zephyr_ld_options(
    -Wl,--wrap=zephyr_getchar_init
    -Wl,--wrap=zephyr_getchar_check
    -Wl,--wrap=zephyr_getchar
  )
elseif(CONFIG_KLAUSSCPU_PYTHON)
  add_subdirectory(ssh)
else()
  return()
endif()

# ── Common to both builds ───────────────────────────────────────────────────

# sys_arch_reboot (standalone), fabs, and the getaddrinfo AAAA wrapper.
zephyr_library_named(klausscpu_mp_port)
zephyr_library_sources(klausscpu_port.c)
if(CONFIG_DNS_RESOLVER AND NOT CONFIG_NET_IPV6)
  zephyr_ld_options(-Wl,--wrap=dns_resolve_name)	# klausscpu_port.c
endif()

# Double-precision libm (musl, as MicroPython's bare-metal ports use it).
set(LIBM_DBL
  __cos.c __expo2.c __fpclassify.c __rem_pio2.c __rem_pio2_large.c
  __signbit.c __sin.c __tan.c acos.c acosh.c asin.c asinh.c atan.c atan2.c
  atanh.c ceil.c cos.c cosh.c copysign.c erf.c exp.c expm1.c floor.c fmod.c
  frexp.c ldexp.c lgamma.c log.c log10.c log1p.c modf.c nearbyint.c pow.c
  rint.c round.c scalbn.c sin.c sinh.c tan.c tanh.c tgamma.c trunc.c
)
list(TRANSFORM LIBM_DBL PREPEND ${MICROPY_DIR}/lib/libm_dbl/)
zephyr_library_named(klausscpu_libm_dbl)
zephyr_library_sources(${LIBM_DBL})
zephyr_library_compile_options(-Wno-double-promotion -Wno-float-conversion
                               -Wno-unused-but-set-variable)

# sqrt: minimal libc has one, but it is not correctly rounded (sqrt(2) is
# 1 ulp low).  Use musl's, under --wrap so it replaces minimal libc's for
# every caller.
zephyr_library_named(klausscpu_libm_sqrt)
zephyr_library_sources(${MICROPY_DIR}/lib/libm_dbl/sqrt.c)
zephyr_library_compile_definitions(sqrt=__wrap_sqrt)
zephyr_ld_options(-Wl,--wrap=sqrt)

# KlaussCPU glue for MicroPython's ports/zephyr (MICROPYTHON_PLAN.md).  Only
# active when the application is that port, which defines
# CONFIG_MICROPY_CONFIGFILE.
set(KLAUSSCPU_MP_DIR ${APPLICATION_SOURCE_DIR}/../..)

# mpconfigport_klausscpu.h, found through MP_CONFIGFILE=<...>.
zephyr_include_directories(.)

# Polled console input: the KlaussCPU UART has no RX interrupt, so wrap the
# port's interrupt-fed zephyr_getchar* (src/zephyr_getchar.c) with a polling
# thread (console_poll.c).
zephyr_library_named(klausscpu_micropython)
zephyr_library_sources(console_poll.c klausscpu_port.c)
zephyr_ld_options(
  -Wl,--wrap=zephyr_getchar_init
  -Wl,--wrap=zephyr_getchar_check
  -Wl,--wrap=zephyr_getchar
)
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
list(TRANSFORM LIBM_DBL PREPEND ${KLAUSSCPU_MP_DIR}/lib/libm_dbl/)
zephyr_library_named(klausscpu_libm_dbl)
zephyr_library_sources(${LIBM_DBL})
zephyr_library_compile_options(-Wno-double-promotion -Wno-float-conversion
                               -Wno-unused-but-set-variable)

# sqrt: minimal libc has one, but it is not correctly rounded (sqrt(2) is
# 1 ulp low).  Use musl's, under --wrap so it replaces minimal libc's for
# every caller.
zephyr_library_named(klausscpu_libm_sqrt)
zephyr_library_sources(${KLAUSSCPU_MP_DIR}/lib/libm_dbl/sqrt.c)
zephyr_library_compile_definitions(sqrt=__wrap_sqrt)
zephyr_ld_options(-Wl,--wrap=sqrt)

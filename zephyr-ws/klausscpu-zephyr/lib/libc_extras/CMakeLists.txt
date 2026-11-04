# C library additions for minimal-libc apps (CONFIG_KLAUSSCPU_LIBC_EXTRAS).
# The headers are added with -I, which is searched before minimal libc's
# -isystem directory, so include/math.h can extend it with #include_next.
zephyr_include_directories(include)
zephyr_library_named(klausscpu_libc_extras)
zephyr_library_sources(${CMAKE_CURRENT_SOURCE_DIR}/../../../../src/setjmp.S)

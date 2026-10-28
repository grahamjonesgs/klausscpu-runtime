# A Zephyr app that must NOT build: zephyr/suite.sh's build step checks that
# its BUILD_ASSERT(0) stops the build, i.e. that the port's C standard (C11,
# arch/klausscpu/Kconfig) keeps Zephyr's build-time checks live.
cmake_minimum_required(VERSION 3.20)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(klausscpu_build_assert_probe)
target_sources(app PRIVATE src/main.c)

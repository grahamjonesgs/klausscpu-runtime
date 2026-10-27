cmake_minimum_required(VERSION 3.20)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(klausscpu_llext_demo)

target_sources(app PRIVATE src/main.c)

# The extension is compiled from ext/hello_ext.c at build time, with the same
# compiler as the kernel, into an ELFCLASS32 ET_REL object (the flags of the
# bare-metal Makefile's %.llext rule, long calls included), and embedded in
# main.c as a byte array.
# Building it here keeps its code in step with the toolchain's instruction
# encoding; a checked-in prebuilt image went stale when the ISA changed.
set(HELLO_EXT_SRC ${CMAKE_CURRENT_SOURCE_DIR}/ext/hello_ext.c)
set(HELLO_EXT_OBJ ${CMAKE_CURRENT_BINARY_DIR}/hello_ext.llext)
add_custom_command(
  OUTPUT ${HELLO_EXT_OBJ}
  COMMAND ${CMAKE_C_COMPILER} --target=${CMAKE_C_COMPILER_TARGET}
          -Os -nostdinc -ffreestanding -fno-builtin
          -mllvm -klausscpu-short-calls=false
          -c ${HELLO_EXT_SRC} -o ${HELLO_EXT_OBJ}
  DEPENDS ${HELLO_EXT_SRC}
  COMMENT "Building LLEXT extension hello_ext.llext"
)
generate_inc_file_for_target(app ${HELLO_EXT_OBJ}
  ${ZEPHYR_BINARY_DIR}/include/generated/hello_ext.inc)

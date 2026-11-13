# CMake toolchain file: KlaussCPU Linux user code (klausscpu-unknown-linux-musl)
# built with klausscpu-llvm against a musl sysroot.
#   KLAUSSCPU_LLVM_BIN  toolchain bin dir
#   KLAUSSCPU_SYSROOT   musl sysroot (lib/libc.so, lib/libc.a, include/)
# Both come from the environment or -D.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR klausscpu)

foreach(v KLAUSSCPU_LLVM_BIN KLAUSSCPU_SYSROOT)
  if(NOT ${v} AND DEFINED ENV{${v}})
    set(${v} "$ENV{${v}}")
  endif()
  if(NOT ${v})
    message(FATAL_ERROR "set ${v}")
  endif()
endforeach()
# try_compile projects see only these.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES KLAUSSCPU_LLVM_BIN KLAUSSCPU_SYSROOT)

set(triple klausscpu-unknown-linux-musl)
set(CMAKE_C_COMPILER "${KLAUSSCPU_LLVM_BIN}/clang")
set(CMAKE_CXX_COMPILER "${KLAUSSCPU_LLVM_BIN}/clang++")
set(CMAKE_ASM_COMPILER "${KLAUSSCPU_LLVM_BIN}/clang")
set(CMAKE_C_COMPILER_TARGET ${triple})
set(CMAKE_CXX_COMPILER_TARGET ${triple})
set(CMAKE_ASM_COMPILER_TARGET ${triple})
# CMake can take clang for a GNU assembler, which drops the target: say it.
set(CMAKE_ASM_FLAGS_INIT "--target=${triple}")
set(CMAKE_AR "${KLAUSSCPU_LLVM_BIN}/llvm-ar" CACHE FILEPATH "")
set(CMAKE_RANLIB "${KLAUSSCPU_LLVM_BIN}/llvm-ranlib" CACHE FILEPATH "")
set(CMAKE_NM "${KLAUSSCPU_LLVM_BIN}/llvm-nm" CACHE FILEPATH "")
set(CMAKE_OBJDUMP "${KLAUSSCPU_LLVM_BIN}/llvm-objdump" CACHE FILEPATH "")
set(CMAKE_STRIP "${KLAUSSCPU_LLVM_BIN}/llvm-strip" CACHE FILEPATH "")
set(CMAKE_SYSROOT "${KLAUSSCPU_SYSROOT}")

set(CMAKE_FIND_ROOT_PATH "${KLAUSSCPU_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

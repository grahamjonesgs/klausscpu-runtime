# wolfssl-toolchain.cmake — CMake cross-compile toolchain for KlaussCPU.
#
# All paths are computed from CMAKE_CURRENT_LIST_FILE so this file works
# correctly even when CMake re-reads it during try_compile() subprocesses
# (which do not inherit parent -D variables).

cmake_minimum_required(VERSION 3.16)

# ── Skip bare-metal link step during compiler probing ────────────────────────
# Without this, CMake tries to link a test executable which fails because there
# is no OS/libc to link against.  STATIC_LIBRARY avoids the linker entirely.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_C_COMPILER_WORKS   1 CACHE BOOL "" FORCE)
set(CMAKE_CXX_COMPILER_WORKS 1 CACHE BOOL "" FORCE)

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR klausscpu)

# ── Path computation ─────────────────────────────────────────────────────────
# Runtime-relative paths come from this file's own location; the toolchain comes
# from the klausscpu-llvm fork via the KLAUSSCPU_LLVM_BIN env var (inherited by
# the try_compile() subprocesses that re-read this file, which do NOT see -D
# variables passed on the parent's command line).
get_filename_component(_WOLFSSL_DIR  "${CMAKE_CURRENT_LIST_FILE}" DIRECTORY)
# _WOLFSSL_DIR  = .../runtime/freertos/wolfssl
get_filename_component(_FREERTOS_DIR "${_WOLFSSL_DIR}/.."  ABSOLUTE)
# _FREERTOS_DIR = .../runtime/freertos
get_filename_component(_RUNTIME_DIR  "${_FREERTOS_DIR}/.." ABSOLUTE)
# _RUNTIME_DIR  = repo root

if(NOT DEFINED ENV{KLAUSSCPU_LLVM_BIN})
    message(FATAL_ERROR
        "KLAUSSCPU_LLVM_BIN is not set. Export it to <klausscpu-llvm>/build/bin "
        "(the built toolchain). See ../../README.md")
endif()
# _LLVM_BUILD = parent of the toolchain bin dir (= <klausscpu-llvm>/build).
get_filename_component(_LLVM_BUILD "$ENV{KLAUSSCPU_LLVM_BIN}/.." ABSOLUTE)
set(_PICOLIBC     "${_RUNTIME_DIR}/picolibc-install")
set(_FREERTOS_INC "${_FREERTOS_DIR}")

# ── Compiler and tools ────────────────────────────────────────────────────────
set(CMAKE_C_COMPILER   "${_LLVM_BUILD}/bin/clang"    CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER "${_LLVM_BUILD}/bin/clang++"  CACHE FILEPATH "" FORCE)
set(CMAKE_AR           "${_LLVM_BUILD}/bin/llvm-ar"  CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB       "${_LLVM_BUILD}/bin/llvm-ranlib" CACHE FILEPATH "" FORCE)

set(_TRIPLE "klausscpu-unknown-elf")

set(_FLAGS
    "-target ${_TRIPLE}"
    " -O1 -Wno-error -Wno-int-conversion -Wno-implicit-int-conversion -ffreestanding -ffunction-sections -fdata-sections -fPIC"
    " -D__IEEE_LITTLE_ENDIAN -D_LDBL_EQ_DBL"
    " -isystem ${_PICOLIBC}/include"
    " -DWOLFSSL_USER_SETTINGS"
    " -I${_WOLFSSL_DIR}"              # user_settings.h
    " -I${_FREERTOS_INC}"            # FreeRTOS.h etc.
    " -I${_FREERTOS_INC}/FreeRTOS-Kernel/include"
    " -I${_FREERTOS_INC}/portable/KlaussCPU"
    " -I${_RUNTIME_DIR}"             # mmio.h (needed by portmacro.h → FreeRTOS.h chain)
)
string(REPLACE ";" " " _FLAGS_STR "${_FLAGS}")

set(CMAKE_C_FLAGS   "${_FLAGS_STR}" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS "${_FLAGS_STR}" CACHE STRING "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS  "-nostdlib" CACHE STRING "" FORCE)

# ── Search paths ──────────────────────────────────────────────────────────────
set(CMAKE_FIND_ROOT_PATH "${_PICOLIBC}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

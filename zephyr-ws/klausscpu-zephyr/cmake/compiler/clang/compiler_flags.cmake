include("${ZEPHYR_BASE}/cmake/compiler/clang/compiler_flags.cmake")

# KlaussCPU: -Os produces incorrect Zephyr kernel code at runtime (the scheduler
# returns garbage switch_handle values, causing IRET to jump to .text and crash).
# The backend compiles -Os without asserting, but the generated code is wrong.
# Force -O2 until the root cause (likely a PIC relocation or calling-convention
# issue under size optimisation) is diagnosed and fixed.
# (2026-10-28, klausscpu-llvm 'size-opts': CONFIG_SIZE_OPTIMIZATIONS_AGGRESSIVE,
# i.e. -Oz, alone and with CONFIG_LTO, builds every app in apps/ plus hello and
# synchronization, and they pass tools/validate/zephyr/suite.sh on the emulator
# and, with LTO, on the board: ssh_shell's text is 32% smaller at -Oz, 40% with
# LTO, for 2-58% slower GUI/doom frames.  -Os itself was not retried.)
set_compiler_property(PROPERTY optimization_size -O2)

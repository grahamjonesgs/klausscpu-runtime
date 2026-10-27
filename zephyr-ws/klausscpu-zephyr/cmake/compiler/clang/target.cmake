include("${ZEPHYR_BASE}/cmake/compiler/clang/target.cmake")

# LLEXT extensions are loaded anywhere in the heap, beyond the +-512 KiB reach
# of the 1-word short CALL, and the loader cannot add the thunks lld would:
# every call in an extension is the 2-word CALLREL / CALL (arch elf.c).
list(APPEND LLEXT_APPEND_FLAGS "SHELL:-mllvm -klausscpu-short-calls=false")

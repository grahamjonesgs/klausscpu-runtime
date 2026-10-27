zephyr_library()
zephyr_library_sources(
    thread.c
    irq.c
    fatal.c
)
zephyr_library_sources_ifdef(CONFIG_CPU_KLAUSSCPU swap.S)
zephyr_library_sources_ifdef(CONFIG_LLEXT elf.c)

# The __sync_*_N libcalls the compiler emits for every C11 / __atomic /
# __sync read-modify-write and compare-and-swap (forced-atomics model: the
# ISA has no atomic RMW).  Shared with the bare-metal and FreeRTOS runtimes;
# each call masks interrupts through INT_MASK, as arch_irq_lock() does.
# Zephyr's own atomic_t API does not use them (CONFIG_ATOMIC_OPERATIONS_C),
# and --gc-sections drops the ones nothing calls.
zephyr_library_sources(${CMAKE_CURRENT_SOURCE_DIR}/../../../../../src/atomic.c)

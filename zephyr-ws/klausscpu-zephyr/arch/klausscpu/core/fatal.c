/*
 * fatal.c — KlaussCPU fatal error handler for Zephyr.
 */

#include <zephyr/kernel.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/fatal.h>

void z_klausscpu_fatal_error(unsigned int reason, const z_arch_esf_t *esf)
{
    z_fatal_error(reason, esf);
}

/*
 * The end of the fatal-error path (k_sys_fatal_error_handler): an abnormal
 * stop, so TRAP rather than HALT.  TRAP parks the core and the SoC prints the
 * crash dump on the board, the emulator stops with `stop = Trap`, and a
 * debugger sees SIGTRAP.  HALT is the clean end of a program (exit status in
 * R12, reported as "exited" by the debugger), which this is not.  The reason
 * code (K_ERR_*) goes in R12, as an exit status would, so it shows as R12 in
 * the crash dump.
 *
 * Spelled out as `trap` rather than __builtin_trap(), so the stop does not
 * depend on how the compiler lowers llvm.trap (it used to be HALT).
 */
FUNC_NORETURN void arch_system_halt(unsigned int reason)
{
    /* Disable all interrupts (INT_MASK = 0), then stop. */
    __asm__ volatile(
        "setr   r0, 0xF00F0000\n\t"
        "setr   r1, 0\n\t"
        "memset32 r1, r0\n\t"
        : : : "r0", "r1", "memory"
    );
    register long r12 __asm__("r12") = (long)reason;
    for (;;) {
        __asm__ volatile("trap" : : "r"(r12));
    }
}

/* mpconfigport_ssh.h — MicroPython configuration for the `python` command in
 * ssh_shell (CONFIG_KLAUSSCPU_PYTHON, MICROPYTHON_PLAN.md step 6).
 *
 * Written for this build rather than wrapping ports/zephyr/mpconfigport.h:
 * there is no UART/BT/sensor/pin layer here, stdio is the SSH session
 * (mphalport.c), and MicroPython threads stay off (the upstream header turns
 * them on whenever CONFIG_THREAD_CUSTOM_DATA is set, which ssh_shell needs
 * for `run`).
 *
 * Shared with the standalone build: setjmp for NLR and the GC register scan,
 * double floats (compiler-rt soft float, lib/libm_dbl), the exact float
 * repr, LP64 sizes.
 */
#include <stdint.h>
#include <alloca.h>
/* ports/zephyr/zephyr_filesystem.c uses the fstab DT macros before it
 * includes anything that brings in devicetree.h. */
#include <zephyr/devicetree.h>

#define MICROPY_CONFIG_ROM_LEVEL (MICROPY_CONFIG_ROM_LEVEL_EXTRA_FEATURES)

/* No assembly for this CPU. */
#define MICROPY_NLR_SETJMP          (1)
#define MICROPY_GCREGS_SETJMP       (1)

/* Core. */
#define MICROPY_ENABLE_GC           (1)
#define MICROPY_ENABLE_FINALISER    (1)
#define MICROPY_STACK_CHECK         (1)
#define MICROPY_STACK_CHECK_MARGIN  (8192)	/* C below Python: print -> wolfSSH -> crypto -> net */
#define MICROPY_ENABLE_EMERGENCY_EXCEPTION_BUF (1)
#define MICROPY_ENABLE_SOURCE_LINE  (1)
#define MICROPY_KBD_EXCEPTION       (1)
#define MICROPY_ENABLE_SCHEDULER    (1)
#define MICROPY_HELPER_REPL         (1)
#define MICROPY_REPL_AUTO_INDENT    (1)
#define MICROPY_PERSISTENT_CODE_LOAD (1)
#define MICROPY_LONGINT_IMPL        (MICROPY_LONGINT_IMPL_MPZ)
#define MICROPY_FLOAT_IMPL          (MICROPY_FLOAT_IMPL_DOUBLE)
/* Float <-> string: the default APPROX.  EXACT relies on long double being
 * wider than double, which it isn't here (LP64, long double = double):
 * only 66% of repr() strings parsed back to the same float
 * (tests/float/float_format_accuracy.py). */
#define MICROPY_FLOAT_FORMAT_IMPL   (MICROPY_FLOAT_FORMAT_IMPL_APPROX)
#define MICROPY_PY_BUILTINS_COMPLEX (1)
#define MICROPY_PY_BUILTINS_HELP    (1)
#define MICROPY_PY_BUILTINS_HELP_TEXT klausscpu_help_text
#define MICROPY_PY_SYS_PLATFORM     "klausscpu"
#define MICROPY_PY_SYS_STDFILES     (1)
#define MICROPY_PY_SYS_MAXSIZE      (1)
#define MICROPY_PY_SYS_EXIT         (1)
#define MICROPY_PY_MICROPYTHON_MEM_INFO (1)
#define MICROPY_USE_INTERNAL_PRINTF (0)   /* keep libc printf (exported to LLEXT) */
#define MICROPY_HW_BOARD_NAME       "Nexys A7"
#define MICROPY_HW_MCU_NAME         "KlaussCPU"
#define MICROPY_BANNER_MACHINE      "KlaussCPU Nexys A7 (Zephyr ssh_shell)"

/* Files: the SD card through Zephyr's fs API (ports/zephyr/zephyr_filesystem.c),
 * mounted at / by the `python` command. */
#define MICROPY_VFS                 (1)
#define MICROPY_READER_VFS          (1)
#define MICROPY_PY_OS               (1)
#define MICROPY_PY_OS_STATVFS       (0)
#define MICROPY_PY_OS_URANDOM       (0)
#define MICROPY_PY_ZEPHYR           (1)

/* Modules. */
#define MICROPY_PY_TIME_TIME_TIME_NS (1)
#define MICROPY_PY_TIME_INCLUDEFILE "ports/zephyr/modtime.c"
#define MICROPY_PY_SOCKET           (1)
#define MICROPY_PY_ERRNO            (1)
#define MICROPY_PY_SELECT           (1)
#define MICROPY_PY_HASHLIB          (1)
#define MICROPY_PY_BINASCII         (1)
#define MICROPY_PY_RANDOM_SEED_INIT_FUNC (klausscpu_random_seed())	/* TRNG */
#define MICROPY_PY_MACHINE          (1)
#define MICROPY_PY_MACHINE_INCLUDEFILE "modmachine_port.c"	/* machine.idle() */
#define MICROPY_PY_MACHINE_MEMX     (1)
#define MICROPY_PY_MACHINE_RESET    (0)
#define MICROPY_PY_MACHINE_PIN_BASE (0)

/* Types. */
typedef long mp_off_t;
#define MP_SSIZE_MAX (0x7fffffffffffffffL)
#define MP_STATE_PORT MP_STATE_VM

/* Interrupt Ctrl-C delivery (the transport's RX hook) and waits. */
void mp_hal_signal_event(void);
#define MICROPY_SCHED_HOOK_SCHEDULED mp_hal_signal_event()

uint32_t klausscpu_random_seed(void);	/* python_cmd.c */

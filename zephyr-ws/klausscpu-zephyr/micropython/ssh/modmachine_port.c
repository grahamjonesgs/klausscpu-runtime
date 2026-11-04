// Port part of the machine module (included by extmod/modmachine.c through
// MICROPY_PY_MACHINE_INCLUDEFILE): only machine.idle().  machine.mem8/16/32
// come from extmod/machine_mem.c.
#include <zephyr/kernel.h>

static void mp_machine_idle(void) {
    k_yield();
}

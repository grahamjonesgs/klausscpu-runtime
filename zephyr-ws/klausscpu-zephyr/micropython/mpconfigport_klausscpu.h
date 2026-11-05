/* mpconfigport_klausscpu.h — MicroPython configuration for KlaussCPU.
 *
 * Selected with CONFIG_MICROPY_CONFIGFILE (micropython/nexys_a7.conf); wraps
 * the upstream ports/zephyr/mpconfigport.h.
 *
 * MicroPython has no assembly for this CPU, so non-local return (Python
 * exceptions) and the GC's register scan both use setjmp (lib/libc_extras,
 * src/setjmp.S).  Floats are doubles, as in CPython; the arithmetic is
 * compiler-rt soft float and the math functions are lib/libm_dbl.
 */
#define MICROPY_NLR_SETJMP    (1)
#define MICROPY_GCREGS_SETJMP (1)

#include "mpconfigport.h"

#undef MICROPY_FLOAT_IMPL
#define MICROPY_FLOAT_IMPL (MICROPY_FLOAT_IMPL_DOUBLE)

/* Float <-> string: the default APPROX.  EXACT relies on long double being
 * wider than double, which it isn't here (long double = double): only 66% of
 * repr() strings parsed back to the same float. */
#define MICROPY_FLOAT_FORMAT_IMPL (MICROPY_FLOAT_FORMAT_IMPL_APPROX)

/* The port assumes 32-bit; this is LP64, so sys.maxsize is 2**63 - 1. */
#undef MP_SSIZE_MAX
#define MP_SSIZE_MAX (0x7fffffffffffffffL)

/* The UART has a 16-byte RX FIFO and no RX interrupt; console_poll.c drains
 * it every tick.  Raw-paste mode (mpremote, pyboard.py) sends at most two
 * flow-control windows of buf/2 bytes before waiting for an ack, so a
 * 16-byte buffer keeps a paste within the FIFO at 3 Mbaud. */
#define MICROPY_REPL_STDIN_BUFFER_MAX (16)

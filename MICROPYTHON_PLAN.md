# MicroPython on KlaussCPU / Zephyr 4.5 — plan

**Status: in progress.** Steps 1–4 done (2026-11-04).  The REPL runs on the emulator and on the board, with networking; next is tests (step 5).

Goal: run Python on the board.  The first target is a MicroPython REPL on the
UART; the second is `python` inside `ssh_shell`, with a REPL over SSH and
scripts run from `/SD:`.

## Why MicroPython

CPython needs a full POSIX OS and tens of MB.  MicroPython is Python 3 for
microcontrollers: about 300 KB of code, a heap of whatever size we give it,
and an upstream Zephyr port (`ports/zephyr`).

## What the port needs from us

| Need | How MicroPython uses it | How we provide it |
|---|---|---|
| Non-local return (Python exceptions) | `nlr` with hand-written assembly per CPU, or the `setjmp` fallback | `MICROPY_NLR_SETJMP`.  Our `src/setjmp.S` (the bare-metal one) |
| GC register scan | `gchelper` with assembly per CPU, or `setjmp` | `MICROPY_GCREGS_SETJMP`.  `setjmp.S` saves R4-R7, R15 and SP, which is the backend's full callee-saved set (`CSR_KlaussCPU_Call`) |
| `<setjmp.h>` | — | Zephyr's minimal libc has none.  Added by the module (`lib/libc_extras`) |
| Floating point | `float` / `double` maths | Soft float from compiler-rt.  `libclang_rt.builtins.a` already has the full SF and DF set, and Zephyr links it (checked: a double/float test gives the right answers on the emulator) |
| `<math.h>` functions | `math` module: `sin`, `pow`, `isnan`, `NAN`, ... | Minimal libc only has `sqrt` and constants.  `lib/libc_extras/include/math.h` adds the C99 declarations on top (`#include_next`).  The code is MicroPython's own `lib/libm_dbl` (musl), compiled by our glue |
| Console input | `zephyr_getchar` fed by `uart_console`'s interrupt-driven input hook | Our UART is polled only (no RX interrupt).  The glue wraps `zephyr_getchar*` (`-Wl,--wrap`) with a polling thread that keeps Ctrl-C working |

Float choice: `MICROPY_FLOAT_IMPL_DOUBLE`.  The upstream port uses `float`, but
Python floats are doubles and soft float costs much the same either way.

Target facts: LP64 (64-bit pointers and `long`), little-endian, `long double`
is `double`.  MicroPython's 64-bit build is the usual Unix one.

## Layout

- MicroPython source: `zephyr-ws/modules/lib/micropython`, pinned at v1.29.0
  (west project in `klausscpu-zephyr/west.yml`, gitignored like the rest of
  `modules/`).  The upstream tree is not patched.
- `klausscpu-zephyr/lib/libc_extras/`: `setjmp.h`, `math.h` and `setjmp.S` for
  minimal-libc apps (`CONFIG_KLAUSSCPU_LIBC_EXTRAS`, off by default).
- `klausscpu-zephyr/micropython/`: the KlaussCPU glue.  It's active when the
  application is MicroPython's `ports/zephyr` (`CONFIG_MICROPY_CONFIGFILE` is
  set):
  - `mpconfigport_klausscpu.h`: the setjmp and float settings, then upstream
    `mpconfigport.h`
  - `nexys_a7.conf`: Kconfig for this board, passed as `EXTRA_CONF_FILE`
  - `console_poll.c`: polled console input
  - `libm_dbl` sources
- Build: `tools/validate/zephyr/build.sh micropython <dir>`.

## Steps

1. **Fetch and toolchain prerequisites.**  MicroPython v1.29.0, `setjmp.h` and
   `setjmp.S` for Zephyr, `math.h` extras, a soft-float check.  *Done.*  The
   libc extras also needed `alloca.h` and `unistd.h` (types only).
2. **First build of `ports/zephyr` for `nexys_a7`.**  Board config: no BT,
   sensors or watchdog; heap 4 MiB; DHCP.  Fix what the compiler and
   linker report.  *Done.*  The fixes were:
   - `nan()`, `fabs`, and `sys_arch_reboot` (no software reset, so
     `machine.reset()` halts);
   - `UART_CONSOLE_DEBUG_SERVER_HOOKS` stays on so `zephyr_getchar.c`
     compiles;
   - the image is 4.8 MB, mostly the 4 MiB heap.
3. **Emulator bring-up.**  Boot to the `>>>` banner on klausscc, then run a
   scripted REPL session over the emulated UART: arithmetic, big ints,
   floats/`math`, exceptions, a GC stress loop.  *Done.*  The emulator's
   `--gdbserver` mode feeds stdin to the UART; a gdb-remote `c` starts it.
   Checked:
   - big ints, closures, classes, comprehensions, recursion, dicts with 20k
     entries;
   - GC collect and `machine.mem32`;
   - Ctrl-C breaking a busy loop, through the polling thread.

   Float findings:
   - Soft-float `+ - * /` and parsing are bit-exact with CPython.
   - Minimal libc `sqrt` was 1 ulp off; musl's replaces it (`--wrap=sqrt`).
   - musl `exp(1)` is 1 ulp from glibc's.  That's library accuracy, not a bug.
   - `repr()` can differ from CPython in the last digit (`1/7` prints
     `0.14285714285714284`).  The string still round-trips to the same bits.
     It's upstream `py/formatfloat.c` when `long double` is `double`, as on
     ARM.
4. **Board bring-up.**  REPL over the UART: Ctrl-C (also during
   `time.sleep`, which the emulator can't show because it fast-forwards idle
   time), paste mode, `machine.mem32` reads (PERF_CYCLES), sockets over the
   board's Ethernet, and speed (pystone-style loop).  *Done.*  Results:
   - Ctrl-C works in a busy loop and during `time.sleep(30)`.
     `sleep_ms(250)` takes 251 ms.
   - Speed:
     - `for` loop with `s += i * i`: ~114k iterations/s;
     - float loop: ~31k iterations/s;
     - `fib(20)`: 357 ms;
     - `str(3**20000)`: 5.2 s (MicroPython's decimal conversion is quadratic).
   - Raw paste through `tools/pyboard.py` at 3 Mbaud: a 9.8 KB script runs
     with the right checksum, at ~1 KB/s (8-byte windows, USB-serial latency).
   - Networking:
     - DHCP lease, DNS (`getaddrinfo('micropython.org', 80)`), and an HTTP
       GET from example.com;
     - a server on the board took a 1 MiB upload at ~585 KB/s while hashing
       it, and the SHA-256 matched.

   Fixes this step needed, three of them outside MicroPython:
   - **UART driver:** sent `\r\n` for every `\n`, so all apps printed
     `\r\r\n` (uart_console already adds the `\r`).  The raw REPL needs
     exact bytes.  The driver now passes bytes through.
   - **Arch, scheduler start:** `_klausscpu_sched_ready` was set at
     APPLICATION 99.  An init function that blocked before that (net_config
     waiting for DHCP at APPLICATION 95) was never switched back in, because
     the timer ISR kept returning to idle.  Now set at POST_KERNEL 0.
   - **Upstream `getaddrinfo`:** raises the AAAA query's EPFNOSUPPORT without
     IPv6.  `--wrap=dns_resolve_name` answers AAAA with NONAME instead.
   - **Config:**
     - 3 Mbaud and a 16-byte RX FIFO, so a 1 kHz tick and a 16-byte REPL
       input buffer (raw-paste window 8);
     - DHCP-only network settings (the port's static 192.0.2.x DNS server took
       the only resolver slot);
     - the Ethernet driver;
     - 16 TCP connections and contexts (sockets waiting for GC used up the
       default 4).
   - Plain (non-raw) paste of more than 16 bytes at once still drops
     characters.  Typing works, and so do `pyboard.py` and `mpremote`.
5. **Tests.**  Run a subset of MicroPython's `tests/` (basics, float, micropython)
   through `tests/run-tests.py --target` style raw-REPL execution, and record
   pass/fail.  Add `micropython` to the Zephyr validation suite (build and
   emulator boot).
6. **`ssh_shell` integration.**  Build the MicroPython core as a library inside
   `ssh_shell`.  Add a `python [file]` shell command that runs a script from
   `/SD:` or a REPL on the SSH channel, with stdio routed through the shell
   transport and the heap from the shell's pool.  Add a FatFs-backed VFS so
   `open()` and `import` see the SD card.

   Design (ssh_shell already does DHCP, NTP, SD mount; Python just uses them):
   - `CONFIG_KLAUSSCPU_PYTHON` (ssh/Kconfig).  `micropython/CMakeLists.txt`
     builds the core from `modules/lib/micropython`: `py/`, the `extmod`
     modules, readline, pyexec, and the port's `modsocket.c`, `modtime.c`,
     `zephyr_filesystem.c` and `modzephyr.c`.  It uses MicroPython's own
     `py.cmake`/`mkrules.cmake` for qstr generation, as `ports/zephyr` does,
     and has its own `mpconfigport_ssh.h`:
     - no `machine` drivers (only `mem32` and friends);
     - no MicroPython threads, which upstream turns on automatically because
       `THREAD_CUSTOM_DATA` is set.
   - `python` runs on the session's shell thread, like `run` (40 KiB stack).
     The heap is `k_malloc`'d per run, 1 MiB by default, added to the pool
     with `HEAP_MEM_POOL_ADD_SIZE_`.
   - One interpreter at a time: MicroPython's state is global.  A second
     session gets "python is in use".
   - I/O:
     - stdout goes through the transport's locked `wolfSSH_stream_send`;
     - stdin comes from `ssh_shell_getc`, and a closed session acts as
       Ctrl-D;
     - Ctrl-C: a per-session RX hook in `ssh_shell_feed`, called on the
       reader thread, turns the interrupt character into
       `mp_sched_keyboard_interrupt()` while Python runs.
   - Files: the SD mount moves to a devicetree `zephyr,fstab,fatfs` entry.
     `ssh_shell` still mounts it from `main.c`, through `FS_FSTAB_ENTRY`.
     Python mounts `zephyr.FileSystem('/SD:')` at `/` and changes into it, so
     `open('x.txt')` and `import foo` work on SD files.
   - On the serial console `python` runs scripts with output to the UART.  A
     REPL there needs UART input, which the shell owns; leave it for later.
7. **Extras (optional).**  A `klausscpu` module with the blitter, the cycle
   counter and the crypto engine.  Frozen `.mpy` modules.

## Risks

- **UART RX FIFO overflow while polling.**  The UART runs at 3 Mbaud
  (300 bytes/ms) with a 16-byte FIFO and no RX interrupt.  Raw paste stays
  safe through flow control with a 16-byte buffer.  A plain paste can't.  An
  RX interrupt or a deeper FIFO in the RTL would fix both.
- **Load time.**  The image's single PT_LOAD segment includes `.bss`, so the
  4 MiB heap's zeros go over the UART: 9.2 MiB takes 33 s.  This applies to
  every Zephyr app's linker layout (ssh_shell's heap too).  A NOLOAD `.bss`
  that Zephyr zeroes itself would cut it to under 1 MB.
- **Upstream port targets Zephyr v4.4.**  Expect small API breaks against 4.5.
- **GC correctness with the setjmp fallback** depends on `setjmp.S` saving
  every callee-saved register.  It does today; if the ABI changes, the GC
  breaks silently.  Run the GC stress test in step 3.

# Debugging KlaussCPU programs with lldb and VS Code

Source-level debugging on the `klausscc` emulator — bare-metal programs,
FreeRTOS and Zephyr: breakpoints, stepping, call stacks (including through
interrupt handlers), locals/globals, registers, watchpoints and RTOS tasks
as threads, in VS Code or the lldb command line — and the same on the real
board (core 1 of the Nexys A7) over its USB-JTAG; see [The board](#the-board).
The plan is `klaussCPU/DEBUG_PLAN.md`.

```
VS Code ─DAP─▶ lldb-dap ─gdb-remote─▶ klausscc --gdbserver ─▶ emulator
                                      klausscc --gdbserver --board ─JTAG─▶ debug module ─▶ core 1
```

## What you need

All three come from the `debug-support` branches:

| Piece | Where | Notes |
|---|---|---|
| `lldb`, `lldb-dap` | `klausscpu-llvm/build/bin` | Stock lldb does not know KlaussCPU. Build with `lldb` in `LLVM_ENABLE_PROJECTS`. |
| `klausscc` with `--gdbserver` | `klausscc/target/release/klausscc` | `cargo build --release` |
| A program built with `-g` | `baremetal/` | `make <name>.elf OPT="-O1 -g"`; `-O0 -g` steps most predictably. |

The scripts and VS Code config look for `klausscpu-llvm` and `klausscc` next
to this repository; set `KLAUSSCPU_LLVM_BIN` and `KLAUSSCC` to override.

## VS Code

1. Install the **LLDB DAP** extension (`llvm-vs-code-extensions.lldb-dap`).
   With Remote-SSH, install it on the remote host.
2. Open the `klausscpu-runtime` folder.
3. Open a program, e.g. `baremetal/programs/dbgtest.c`, set breakpoints in the
   margin, and press **F5** ("KlaussCPU: debug the open bare-metal program on
   the emulator").

F5 runs the `klausscpu: emulator gdbserver` task (`debug/emu-gdbserver.sh`):
it rebuilds the program with `-O1 -g`, then serves it on port 1234.  The
program's UART output appears in that task's terminal, and typing there feeds
the UART input.  lldb-dap then attaches with `gdb-remote 1234`.  Use the
Debug Console for lldb commands (`register read`, `memory read`, `watchpoint
set variable counter`, …).

The launch configuration passes lldb-dap's path explicitly
(`debugAdapterExecutable`), so the extension's own lldb-dap setting is not
used.

## Command line

```sh
debug/emu-gdbserver.sh dbgtest           # terminal 1: build + serve on :1234
../klausscpu-llvm/build/bin/lldb          # terminal 2
(lldb) target create baremetal/dbgtest.elf
(lldb) gdb-remote 1234
(lldb) b dist2
(lldb) c
(lldb) bt
(lldb) frame variable
```

`klausscc --input prog.elf --gdbserver PORT` serves any ELF linked at 0x20
(the normal `klausscpu.ld` layout).

## Behaviour

- **Breakpoints:** unlimited software breakpoints, and 4 hardware ones
  (`b -H`), like the planned board debug module.  Breakpoints are emulated,
  so code is never patched.
- **Watchpoints:** 2, each up to an aligned 8 bytes, reported after the
  access.
- **`__builtin_debugtrap()`** and **`__builtin_trap()`** (both the `trap`
  instruction) stop with SIGTRAP on the trap; continuing steps over it.
  A Zephyr fatal error ends in `trap` too (`arch_system_halt`, reason code
  in `r12`).
- **HALT** ends the session: "exited" with the low byte of `r12`, which is
  main's return value when `crt0` halts.
- **Faults:** an invalid instruction stops with SIGILL, and a PC outside RAM
  with SIGSEGV.
- **MMIO** (`0xF000_0000+`) is not readable from the debugger, because some
  device reads have side effects (for example, reading UART RX pops the
  FIFO).  `process plugin packet monitor mmio on` allows it.
- **Registers:** `r0`–`r15` (aliases `a`–`p`), `sp`, `pc`, `flags`
  (Z/S/C/V fields) and `int_mask`.
- **Optimised code:** at `-O1`, variables show "not available" where the
  value is not live (for example, before it is computed, or after its
  register is reused).  That is expected; use `-O0` when that gets in the
  way.

## RTOS programs

The emulator models the interrupt controller and timer (`0xF00F_0000`) and
`WAIT`, so FreeRTOS and Zephyr images run on it as well as on the board.

**VS Code:** pick one of the RTOS launch configurations and press F5 (any
file can be open); it asks which program to debug, builds it with debug
info, and serves it:

| Configuration | Asks for | Builds |
|---|---|---|
| FreeRTOS program on the emulator / board (JTAG) | `demo`, `console_demo`, `net_demo` | `freertos/<program>.elf` with `make -B … DBG=-g` |
| Zephyr app on the emulator / board (JTAG) | an app: `klausscpu-zephyr/apps/<app>` (`irq_soak`, `mandel`, …), a `zephyr/samples/` name (`synchronization`, `hello_world`) or a path | `zephyr-ws/build_dbg_<app>` (incremental; delete it for a pristine build) with `CONFIG_DEBUG_INFO`, `CONFIG_THREAD_MONITOR`, `CONFIG_THREAD_NAME` |

`net_demo` and `console_demo` use Ethernet, which only the board has.  The
first Zephyr build of an app takes a minute or two.  Command line: the same
script, `debug/rtos-gdbserver.sh freertos demo 1234` or
`debug/rtos-gdbserver.sh zephyr synchronization 1234` (`BOARD=1` for the
board), then `lldb -o 'gdb-remote 1234' <elf>` (the script links the ELF to
`debug/.rtos.elf`).  Any other ELF linked at 0x20 works too:
`klausscc --input <elf> --gdbserver 1234`, then the "attach to a running
gdbserver" configuration.

- **Threads.** `debug/klausscpu_rtos.py` is an lldb OS plugin that shows
  every FreeRTOS task / Zephyr thread as a debugger thread, named with its
  state (`TaskA [Blocked]`, `thread_b [Pending, prio 7]`), each with its own
  call stack.  The launch configurations load it; from the command line:
  `settings set target.process.python-os-plugin-path debug/klausscpu_rtos.py`
  before `target create`.  Zephyr needs `CONFIG_THREAD_MONITOR=y` (and
  `CONFIG_THREAD_NAME=y` for names).
- **Interrupts.** Breakpoints inside handlers work, and the call stack
  continues through `vPortTickISR` / `z_klausscpu_timer_isr` into the
  interrupted task.  The handlers' `.cfi_signal_frame` marks them as trap
  handlers (a `.debug_frame` CIE with the augmentation "S"), so the
  interrupted frame is shown at the interrupted instruction itself, and the
  handler's own frame at the instruction after its call.  Single-stepping never enters an interrupt handler; a
  pending interrupt is taken on the next continue.  `process plugin packet
  monitor irq` prints the interrupt controller and timer state.
- **Timing.** The emulator counts 2 cycles per instruction, so ticks arrive
  at a different instruction count than on the board.
- **Switched-out threads** start at the instruction after their call into
  the context switch (`vPortYield`, `arch_switch`): the OS plugin cannot
  tell lldb that this PC is a return address, so in optimised code that
  instruction's line can show as 0 (`do_swap ... at kswap.h:0`).

## The board

The same debugging, on core 1 of the Nexys A7, through the on-chip debug
module (`klaussCPU` `debug_module.sv`) over the board's USB-JTAG (FT2232H
channel A; the console UART on channel B keeps working).

You also need:

- **A `DEBUG_EN=1` bitstream:** `klaussCPU/tools/build_debug.sh` (about
  15 minutes; `KlaussCPU.sv` defaults to `DEBUG_EN = 0`), then
  `openFPGALoader -b nexys_a7_100 <bit>`.  Programming the FPGA over JTAG is volatile:
  a power cycle goes back to the flash image.
- **klausscc with board support:** `cargo build --release --features board`
  (links libftdi1: `sudo apt install libftdi1-dev`).
- **Vivado's hardware manager closed** — `hw_server` holds the JTAG channel.

VS Code: open a program under `baremetal/programs/` and start **KlaussCPU:
debug the open bare-metal program on the board (JTAG)**.  The task builds it,
loads it over the UART with halt-on-load set, and serves core 1 halted on
the program's first instruction.

Command line:

```sh
debug/board-gdbserver.sh dbgtest 1234          # build, load, halt at entry, serve
ATTACH=1 debug/board-gdbserver.sh dbgtest 1234 # halt the program already running
lldb baremetal/dbgtest.elf -o 'gdb-remote 1234'
```

Differences from the emulator:

- **Breakpoints.** 4 hardware PC comparators (`b -H`), and unlimited
  software breakpoints: the word is replaced by `TRAP` while set (memory
  reads show the original).  Code in ROM or code copied at run time needs
  hardware breakpoints.
- **Watchpoints** (2, up to 8 bytes in one doubleword) halt up to two
  instructions after the access; the reported address is the access's.  A
  store just before a `ret` therefore reports in the caller.
- **Halted = frozen.**  The timer, millisecond clock and cycle counter stop
  while the core is halted, so a tick doesn't fire the instant you continue.
  Single steps run with interrupts masked, as on the emulator.
- **Program output** goes to the console UART (`klausscc -m -s`, or any
  terminal on the second ttyUSB), not the gdbserver's stdout.
- **Detach** removes the breakpoints and lets the program run on.  A program
  that halts (`HALT`) stays halted; load the next one as usual.
- **Memory** reads run at about 200 KiB/s (64 KiB in 0.3 s at the default
  10 MHz JTAG clock), D-cache coherent.
- **RTOS threads at the entry stop.**  Before `_start` clears `.bss`, RAM
  still holds the previous program's data, so if that was the same RTOS
  image the thread list shows its old tasks until the program reaches
  `main`.  Continue (or break on `main`) first.
- **If the server is killed** without detaching, its breakpoints stay armed
  until the next `--board` session or the next ordinary UART load (which
  clears all debugger state), or the CPU reset button.

## Limitations (for now)

- **No calling target functions** from expressions (`p foo()`).  Reading
  variables, conditional breakpoints and arithmetic expressions work.
- **Blitter and Ethernet interrupts** are not modelled by the emulator.

## Regression tests

```sh
debug/run_debug_smoke.sh
```

This builds `dbgtest`, then runs a scripted lldb session (`debug/smoke.lldb`)
and a VS Code-style DAP session (`debug/dap_smoke.py`) against the emulator,
and checks each result.  `debug/run_board_smoke.sh` runs the same lldb
session against the board (connected, `DEBUG_EN=1` bitstream loaded).
`debug/run_rtos_smoke.sh` (`BOARD=1` for the board) builds the FreeRTOS demo
and the Zephyr synchronization sample the way the RTOS launch configurations
do and checks them through lldb-dap (`debug/dap_rtos.py`): a source
breakpoint, every task/thread named with its state, a stack for each.

The validation harness has the end-to-end version,
`tools/validate/validate.sh lldb` (emulator; also part of `full`) and
`lldb-board` (`tools/validate/lldb_e2e.sh`, scripted with `lldb -b -s`:
`tools/validate/lldb/*.lldb`).  It runs `dbgtest` built at -O0, -O1 and -O2
(line and function breakpoints, step-in / finish / next, backtraces
through frameless, shrink-wrapped and tail-calling functions, variables and
registers of outer frames held in callee-saved registers, expressions), and
the Zephyr synchronization sample and the FreeRTOS demo (a backtrace
through the timer interrupt into the interrupted code, the thread list with
a stack per thread, expressions over kernel objects), and prints a
PASS/FAIL line per check.

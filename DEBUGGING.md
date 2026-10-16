# Debugging KlaussCPU programs with lldb and VS Code

Source-level debugging of bare-metal programs on the `klausscc` emulator:
breakpoints, stepping, call stacks, locals/globals, registers and watchpoints,
in VS Code or the lldb command line.  The plan, including the move to the
real board over JTAG, is `klaussCPU/DEBUG_PLAN.md`.

```
VS Code ─DAP─▶ lldb-dap ─gdb-remote─▶ klausscc --gdbserver ─▶ emulator
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
- **`__builtin_debugtrap()`** (the `trap` instruction) stops with SIGTRAP on
  the trap; continuing steps over it.  `__builtin_trap()` is still `halt`.
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

## Limitations (for now)

- **No interrupts or timer:** the emulator does not model them yet, so
  FreeRTOS and Zephyr programs cannot be debugged on it until DEBUG_PLAN
  Phase 3.
- **No calling target functions** from expressions (`p foo()`).  Reading
  variables, conditional breakpoints and arithmetic expressions work.
- **No RTOS thread view yet** (Phase 4).

## Regression test

```sh
debug/run_debug_smoke.sh
```

This builds `dbgtest`, then runs a scripted lldb session (`debug/smoke.lldb`)
and a VS Code-style DAP session (`debug/dap_smoke.py`) against the emulator,
and checks each result.

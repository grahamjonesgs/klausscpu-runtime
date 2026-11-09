# Rust programs for the board

Each folder here is a Rust program that runs on the board as a Zephyr LLEXT
extension: ssh_shell loads it from the SD card with `run`, as it does the C
extensions.

## Writing and running one

- **New program:** `tools/klauss-rust new <name>` (VS Code: Tasks: Run Task ->
  "klausscpu: Rust: new project") makes `rust/<name>/src/lib.rs` and adds it to
  `Cargo.toml` here.
- **Run it:** open any file of the project and press Ctrl+Shift+B ("klausscpu:
  run on board"), or `tools/klauss-rust run rust/<name>`. It builds
  `rust/<name>/<name>.llext`, copies it to `/SD:/rust/` over SFTP, runs it in
  a new SSH session and shows the output. The exit status is the program's,
  and compile errors go to VS Code's Problems panel.
- **On the board afterwards:** `run rust/<name>.llext`. Names with `-` become
  `_` (`my-prog` -> `my_prog.llext`).
- **Build only:** `tools/klauss-rust build rust/<name>`.

ssh_shell must be running on the board. The board settings are
`KLAUSSCPU_BOARD_IP`, `KLAUSSCPU_SSH_USER` and `KLAUSSCPU_SSH_PASS`, as for
`tools/validate/zephyr`.

## What a program can use

- **Libraries:** `#![no_std]`, so `core` and `alloc` are available (`Vec`,
  `String`, `BTreeMap`, `Box`, `Rc`/`Arc`, `format!`, atomics), but `std`
  (files, threads, sockets, `HashMap`) is not.
- **Output:** `klauss_io::println!` writes to the SSH session.
- **Entry point:** `klauss_llext_rt::entry!(run)` names the function that runs.
  It returns the exit code, and it cannot take arguments yet.
- **Calling C:** a program may call only what the kernel exports
  (`zephyr-ws/klausscpu-zephyr/ssh/llext_exports.c`). The build checks every
  import against that list.
- **Panics:** a panic prints its message, then parks that SSH session's thread
  until the board reboots (there is no `llext_exit` yet). Use other sessions
  meanwhile.

## Where things come from

- **Compiler and crates:** the compiler is the patched `rustc`
  (`klausscpu-p5-rust-build/klausscpu-rust`, stage1), built against the
  klausscpu-llvm fork. The target spec, the `klauss-*` crates (`klauss-io`,
  `klauss-llext-rt`, `klauss-mmio`, ...) and the build driver `klauss-build`
  live in the klausscpu-llvm checkout next to this one, under
  `llvm/lib/Target/KlaussCPU/rust/`.
- **Rebuilding the compiler:** after a backend change, rebuild it as that
  folder's `README.md` describes. `tools/klauss-rust` notices a rebuilt
  compiler and starts `rust/target/` afresh, because cargo's cache would not
  notice it.

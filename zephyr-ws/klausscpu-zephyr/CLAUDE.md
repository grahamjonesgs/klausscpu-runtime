# Building KlaussCPU Zephyr programs with `west` — agent guide

Command-first guide for building Zephyr images for the KlaussCPU (custom 32-bit
soft-core, machine type `0x4b43`). For background/rationale see [README.md](README.md);
this file is the "just build it" path. **All commands run from `zephyr-ws/`**
(the west workspace root), one level up from this module.

## 0. Prerequisite — the toolchain (one env var)

The compiler lives in the separate **`klausscpu-llvm`** fork; this repo only has
the software. Every build needs it on the `PATH`-like env var:

```sh
export KLAUSSCPU_LLVM_BIN=<klausscpu-llvm>/build/bin   # dir with clang, ld.lld, llc, llvm-*
```

If it's unset every build entry point fails loudly. The fork must already be
built (see its own README). Confirm: `"$KLAUSSCPU_LLVM_BIN/clang" --version`.

## 1. One-time workspace setup (fresh clone only)

The Zephyr tree and modules are **fetched, not committed** (gitignored). On a
fresh clone, from `zephyr-ws/`:

```sh
# a) Zephyr v3.7.0 + the fatfs module (manifest = klausscpu-zephyr/west.yml)
pip install --user west
west init -l klausscpu-zephyr
west update                         # pulls zephyr/ (~1.1 GB) + fatfs + lvgl

# b) Re-apply the three vendored Zephyr patches AFTER EVERY `west update`
#    (the zephyr/ tree is gitignored, so the patches don't persist there).
git -C zephyr apply ../klausscpu-zephyr/zephyr-patches/klausscpu-core.patch
git -C zephyr apply ../klausscpu-zephyr/zephyr-patches/llext-klausscpu.patch
git -C zephyr apply ../klausscpu-zephyr/zephyr-patches/cbprintf-klausscpu.patch
#    Verify: `git -C zephyr status` shows exactly 11 changed files.
#    (cbprintf-klausscpu.patch: adds klausscpu to the VA_STACK_ALIGN table so
#     cbprintf packaging aligns 32-bit varargs correctly — fixes LOG_*/printk
#     multi-arg %u/%d garble.  Only needed for the lvgl/gui apps but harmless.)

# c) wolfSSL + wolfSSH (only needed for the ssh_shell app) — built from source
#    as Zephyr modules. build-wolfssl.sh also pins the module name to `wolfssl`
#    (upstream module.yml has none, which otherwise breaks wolfSSH's dependency).
( cd ../freertos/wolfssl && ./build-wolfssl.sh && ./build-wolfssh.sh )
```

Already-set-up workspace? Skip this section. Re-applying patches is only needed
after a fresh `west update`.

## 2. Build an app

The two KlaussCPU apps live in `klausscpu-zephyr/apps/` (`ssh_shell`,
`llext_demo`). Builds use the `klausscpu-clang` toolchain variant and pass the
module/root dirs explicitly. Helper vars (run once per shell, from `zephyr-ws/`):

```sh
MOD=$PWD/klausscpu-zephyr
ZEPHYR=$PWD/zephyr
WOLFSSL=$PWD/../freertos/wolfssl/wolfssl-src      # source tree (a Zephyr module)
WOLFSSH=$PWD/../freertos/wolfssl/wolfssh-src
```

### ssh_shell (full app: networking + SSH + HTTPS + FatFs + LLEXT)
Verified-working invocation. `-p always` forces a clean (pristine) reconfigure;
drop it for incremental rebuilds.

```sh
west build -p always -b nexys_a7 "$MOD/apps/ssh_shell" --build-dir build_ssh -- \
  -DZEPHYR_TOOLCHAIN_VARIANT=klausscpu-clang \
  -DKLAUSSCPU_LLVM_BIN="$KLAUSSCPU_LLVM_BIN" \
  -DTOOLCHAIN_ROOT="$MOD" \
  "-DEXTRA_ZEPHYR_MODULES=$MOD;$WOLFSSL;$WOLFSSH" \
  "-DARCH_ROOT=$MOD;$ZEPHYR" "-DSOC_ROOT=$MOD;$ZEPHYR" \
  "-DBOARD_ROOT=$MOD;$ZEPHYR" "-DDTS_ROOT=$MOD;$ZEPHYR"
```

→ `build_ssh/zephyr/zephyr.elf`. Incremental rebuild after editing sources:
`ninja -C build_ssh` (or re-run `west build` without `-p always`).

### A plain sample (no wolfSSL) — e.g. hello_world
Drop the wolfSSL/wolfSSH entries from `EXTRA_ZEPHYR_MODULES`; point the source at
a sample inside the fetched `zephyr/` tree:

```sh
west build -p always -b nexys_a7 zephyr/samples/hello_world --build-dir build_hello -- \
  -DZEPHYR_TOOLCHAIN_VARIANT=klausscpu-clang \
  -DKLAUSSCPU_LLVM_BIN="$KLAUSSCPU_LLVM_BIN" \
  -DTOOLCHAIN_ROOT="$MOD" -DEXTRA_ZEPHYR_MODULES="$MOD" \
  "-DARCH_ROOT=$MOD;$ZEPHYR" "-DSOC_ROOT=$MOD;$ZEPHYR" \
  "-DBOARD_ROOT=$MOD;$ZEPHYR" "-DDTS_ROOT=$MOD;$ZEPHYR"
```

## 3. Verify / load

```sh
"$KLAUSSCPU_LLVM_BIN/llvm-readelf" -h build_ssh/zephyr/zephyr.elf | grep Machine   # → 4b43
```

To run on the FPGA, convert the ELF to the loader's `.kbt` format with `klausscc`
(separate Rust tool, `~/Documents/src/rust/klausscc/`), then load over serial:

```sh
( cd build_ssh/zephyr && klausscc -e zephyr.elf )                 # → zephyr.kbt
klausscc -e build_ssh/zephyr/zephyr.elf --serial /dev/tty.usbserial-... --monitor
```

## 4. LLEXT runtime extensions (the SSH `run` command)

`ssh_shell` can load `.llext` ELF objects at runtime. Build them from the
**bare-metal** Makefile (not west), then copy to the SD card and `run <name>.llext`
over SSH:

```sh
( cd ../baremetal && make ext-demos )   # builds hello/adventure/expr/... .llext
```

Add a program: drop it in `baremetal/programs/`, add its name to `EXT_DEMOS` in
`baremetal/Makefile`. It may call any symbol exported in `ssh/llext_exports.c`
(printf/puts/putchar/getchar, malloc family, mem*/str*) plus the `mmio.h` helpers.

## Gotchas (things that bite)

- **Re-apply the three patches after every `west update`** — the `zephyr/` tree is
  gitignored and fetched clean each time, so the patches don't survive.
- **`EXTRA_ZEPHYR_MODULES` for `ssh_shell` must include both wolfSSL and wolfSSH**
  or Kconfig aborts with `undefined symbol WOLFSSL`. They point at the *source*
  trees (`wolfssl-src`/`wolfssh-src`), not the install dirs.
- **wolfSSL module name:** upstream `module.yml` has no `name:`, so Zephyr derives
  it from the directory (`wolfssl-src`) and wolfSSH's `depends: [wolfssl]` goes
  unmet. `build-wolfssl.sh` injects `name: wolfssl` to fix this — so always build
  wolfSSL via that script, don't hand-clone.
- **`west build` needs `KLAUSSCPU_LLVM_BIN` in the environment** (the toolchain
  cmake reads it from the env during `try_compile`), even though it's also passed
  with `-D`.
- **Pristine vs incremental:** `-p always` wipes the build dir. For a quick
  recompile after editing one file, omit it and use `ninja -C <build-dir>`.
- Build details, Kconfig requirements, memory map and the LLEXT internals are in
  [README.md](README.md).
```

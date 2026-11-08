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
# a) Zephyr v4.5.0 + modules (manifest = klausscpu-zephyr/west.yml), in a
#    venv with Zephyr's Python requirements
python3 -m venv .venv && . .venv/bin/activate && pip install west
west init -l klausscpu-zephyr
west update --narrow -o=--depth=1   # zephyr/ + fatfs, lvgl, mbedtls, tf-psa-crypto(-drivers),
                                    # and modules/lib/micropython (the `python` command)
pip install -r zephyr/scripts/requirements-base.txt
# The shallow clone has no tags, so the boot banner would show a hash:
git -C zephyr fetch --depth=1 https://github.com/zephyrproject-rtos/zephyr tag v4.5.0

# b) Re-apply the vendored patches AFTER EVERY `west update`
#    (zephyr/ and modules/ are gitignored, so the patches don't persist).
for p in klausscpu-core llext-klausscpu cbprintf-klausscpu minimal-libc-strcasecmp \
         ssh-ecdsa-host-key; do
  git -C zephyr apply ../klausscpu-zephyr/zephyr-patches/$p.patch
done
git -C modules/crypto/tf-psa-crypto apply \
  ../../../klausscpu-zephyr/zephyr-patches/tf-psa-crypto-klausscpu.patch
#    Verify: `git -C zephyr status --short` lists 20 paths (one new file).
#    klausscpu-core: the arch dispatch in cpu.h, arch_inlines.h, exception.h,
#      linker-tool-gcc.h, toolchain/common.h and gcc.h.
#    llext-klausscpu: LLEXT_ELF_CLASS32, keep_symtab, arch_elf_check_header,
#      and no alignment pre-padding from a .bss's nominal file offset.
#    cbprintf-klausscpu: 8-byte vararg slots (LOG_*/printk multi-arg output).
#    minimal-libc-strcasecmp: the net shell's dns.c needs strcasecmp.
#    ssh-ecdsa-host-key: ecdsa-sha2-nistp256 server host keys for Zephyr's
#      SSH (CONFIG_SSH_HOST_KEY_ECDSA); a login takes ~1 s against ~4 s RSA.
#    tf-psa-crypto-klausscpu: 64-bit bignum limbs (RSA-2048 sign 3.1 s ->
#      1.1 s, X25519 and P-256 ~1.5x), and the dispatch hooks for the
#      KlaussCPU PSA driver (drivers/crypto: SHA-256 / HMAC on the SHA block).

# c) wolfSSL + wolfSSH (only needed for the ssh_shell app) — built from source
#    as Zephyr modules. build-wolfssl.sh also pins the module name to `wolfssl`
#    (upstream module.yml has none, which otherwise breaks wolfSSH's dependency).
#    build-wolfssh.sh applies zephyr-patches/wolfssh-sftp-zephyr.patch (the SFTP
#    server's Zephyr fixes); tools/validate/zephyr/build.sh refuses a tree
#    without it.  On an existing tree, run build-wolfssh.sh again.
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
**bare-metal** Makefile (not west), then copy to the SD card's `llext/` folder
and `run <name>.llext` over SSH (`run` and `svc load` look for a bare name at
the top of the card, then in `/SD:/llext/`):

```sh
( cd ../baremetal && make ext-demos )   # builds hello/adventure/expr/... .llext
```

`../tools/validate/zephyr/llext_sd.sh <outdir>` does the whole round trip on
the board: it rebuilds the extensions (and ssh_shell's `apibackend.llext` /
`ctrl.llext`), backs up every `.llext` on the card, uploads the new ones and
runs each against its bare-metal baseline.  Rebuild the card's extensions
whenever the toolchain's encoding or ABI changes: the loader refuses an
extension built for another C ABI version (the low byte of `e_flags`) and
`run` prints why.

Add a program: drop it in `baremetal/programs/`, add its name to `EXT_DEMOS` in
`baremetal/Makefile`. It may call any symbol exported in `ssh/llext_exports.c`
(printf/puts/putchar/getchar, malloc family, mem*/str*, and the `__sync_*_N`
atomics) plus the `mmio.h` helpers.

## 5. Python (MicroPython) — the `python` command

`ssh_shell` includes MicroPython v1.29 (`CONFIG_KLAUSSCPU_PYTHON`, source in
`modules/lib/micropython` from west, glue in `micropython/ssh/`).  Over SSH:

```text
$ python                    # REPL; Ctrl-C interrupts, Ctrl-D exits
$ python script.py a b      # run a script from the SD card (paths as the shell's)
```

The SD card is Python's `/`, and Python starts in the shell's cwd, so
`open()` and `import` see SD files (also `/lib`).  One session can use Python
at a time.  A standalone MicroPython image (REPL on the UART, for bring-up
and tests) builds with `../tools/validate/zephyr/build.sh micropython <dir>`.
`asyncio` is frozen in (`micropython/ssh/manifest.py`).  Fixes to upstream
port files live in `micropython/patches/` and are applied to build-time
copies; the fetched tree stays clean.

Testing against the board (ssh_shell running):

```sh
../tools/validate/zephyr/mp_tests.sh <outdir>         # MicroPython's test suite
python3 modules/lib/micropython/tools/pyboard.py \
  -d exec:"python3 ../tools/validate/zephyr/ssh_python.py" script.py
```

Plan, results and caveats: `../MICROPYTHON_PLAN.md`.

## 6. The SD card as a folder (SFTP)

With `sshfs` installed, `../tools/sdcard-mount.sh` mounts the card at
`~/sdcard` (`-u` unmounts), so an editor can open it; `.vscode/tasks.json` has
tasks for this and for running the open file on the board.  wolfSSH's Zephyr
SFTP server needed `zephyr-patches/wolfssh-sftp-zephyr.patch` for this.  To
check a card and server (a second mount adds two-session tests):

```sh
../tools/validate/zephyr/sd_mount_test.py ~/sdcard
```

The HTTP (:80) and HTTPS (:443) file servers only read: uploads (`PUT`) are
off unless `CONFIG_KLAUSSCPU_HTTP_PUT=y`, as they need no login.

## 7. Private keys: QSPI flash keystore

The SSH host key and the TLS key and certificate (`ssh/keystore.h`) live in
the board's QSPI configuration flash when the bitstream has klaussCPU's
`qspi_ctrl` (MMIO 0xF012_0000).  They are stored with NVS on
`storage_partition` (0xF0_0000, 768 KiB, above the FPGA bitstream; see
`nexys_a7.dts`), so they survive reboots, SD card swaps and bitstream
updates (`openFPGALoader -f` only erases the bitstream's sectors).  Driver:
`drivers/flash/flash_klausscpu_qspi.c`.  The hardware refuses to program or
erase below 0xF0_0000.

- **First boot on a qspi bitstream:** keys found only on the card
  (`/SD:/sys/`, or the top of the card from older firmware) are copied into
  flash, so the board keeps its identity.  The card copies stay in place.
  `keystore status` shows where each key is.  `keystore purge_sd` deletes
  card copies that are byte-identical to the flash ones.
- **Bitstream without `qspi_ctrl`:** the flash device is not ready, and the
  keys stay in `/SD:/sys/` (which HTTP/HTTPS refuse) as before.  If the card
  copies have been purged, such a bitstream generates new keys, so the host
  key changes between the two bitstreams.
- **NVS on-flash format:** `CONFIG_NVS_DATA_CRC=y`.  Changing it makes the
  stored keys read as missing.
- **Full-flash dumps:** a 16 MiB `openFPGALoader --dump-flash` image contains
  the private keys.  Restoring one with `-o 0` rolls the keys back as well.

Limits: the patch protects files only between SFTP sessions.  Don't write one
file through SFTP and the shell, HTTP or Python at the same time.  A
file's date and owner can't be set, so all files show as 1970 and owned by you.

## Gotchas (things that bite)

- **Re-apply the patches after every `west update`** — `zephyr/` and
  `modules/` are gitignored and fetched clean each time, so the patches don't
  survive.
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

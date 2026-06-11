# klausscpu-runtime

The **software layer** for the KlaussCPU — a custom 32-bit soft-core CPU with its
own LLVM backend. This repo holds everything written *with* the toolchain: a
bare-metal C runtime + test programs, a **FreeRTOS** port (with lwIP, wolfSSL,
wolfSSH, FatFs), and a **Zephyr** port (`zephyr-ws/klausscpu-zephyr`, including an
SSH shell app with loadable LLEXT extensions).

It was split out of the `klausscpu-llvm` fork so the ~8 MB of software isn't
dragged around by the 3.6 GB compiler history. **It is not self-contained: it
needs a *built* `klausscpu-llvm` toolchain.**

## The contract: one env var

The only link to the compiler is the env var `KLAUSSCPU_LLVM_BIN`, pointing at the
fork's built `bin/` directory (no submodule):

```sh
export KLAUSSCPU_LLVM_BIN=<klausscpu-llvm>/build/bin
```

Every build entry point (`Makefile`, `freertos/Makefile`, `build-*.sh`, the
wolfSSL/Zephyr cmake toolchains) reads this and **fails loudly if it is unset**.
The fork root (`$KLAUSSCPU_LLVM_BIN/../..`) is also where `compiler-rt/lib/builtins`
is found for the soft-FP / integer-divide `crt-*.o` objects.

**Pinned compiler:** built from `klausscpu-llvm` branch `main`, commit
`ebc99da5` (`Prep for repo split docs`) or newer. The backend is stable; any
recent build of the fork works.

## Prerequisites

- A built `klausscpu-llvm` fork — its `build/bin/` must contain `clang`, `llc`,
  `ld.lld`, `llvm-objcopy`, `llvm-objdump`, `llvm-readelf`, `llvm-ar`, `llvm-nm`,
  `llvm-ranlib`, `llvm-strip`, `llvm-size`. See the fork's own README to build it.
- Host tools: `make`, `meson` + `ninja` (picolibc), `cmake` ≥ 3.16 (wolfSSL),
  `west` + `pip` (Zephyr), `git`.

## Fresh-clone setup (fetched, not committed)

The vendored upstreams are **gitignored** — a fresh clone has only the ~8 MB of
glue. Re-fetch what you need, in order, with `KLAUSSCPU_LLVM_BIN` exported:

```sh
export KLAUSSCPU_LLVM_BIN=<klausscpu-llvm>/build/bin

# 1. picolibc — required by every target (clones, builds, installs to picolibc-install/)
./build-picolibc.sh

# 2. compiler-rt builtins archive — only for the Zephyr/driver-linked builds
#    (the bare-metal Makefile links individual crt-*.o and does NOT need this)
./build-builtins.sh

# 3. lwIP — only for the network demos (clones into lwip/)
./get-lwip.sh

# 4. FreeRTOS kernel — for the freertos/ targets
cd freertos && ./get-freertos.sh && cd ..

# 5. wolfSSL + wolfSSH — for the FreeRTOS TLS/SSH demos and the Zephyr ssh_shell app
cd freertos/wolfssl && ./build-wolfssl.sh && ./build-wolfssh.sh && cd ../..

# 6. Zephyr workspace — for the Zephyr port (see zephyr-ws/klausscpu-zephyr/README.md)
cd zephyr-ws
pip install --user west
west init -l klausscpu-zephyr
west update                              # fetches zephyr/ (v3.7.0) + west modules
# then re-apply the two vendored Zephyr patches (see the Zephyr README)
cd ..
```

## Building

### Bare-metal C programs
```sh
make hello          # uart hello-world
make test_fp        # soft-FP test (links compiler-rt crt-*.o from the fork)
make all            # every .elf
```
Output is a `*.elf` with machine type `0x4b43` ("KC"). See the top of `Makefile`
for the full target list (adventure, expr, bst, crypto, queens, fs_demo, …).

### FreeRTOS
```sh
cd freertos
make demo           # task demo, no network
make net_demo       # FreeRTOS + lwIP: DHCP + NTP + HTTP
```

### Zephyr (SSH shell)
Full workspace setup and the exact `west build` invocation (including the
wolfSSL/wolfSSH `EXTRA_ZEPHYR_MODULES` and the two required Zephyr patches) live
in [`zephyr-ws/klausscpu-zephyr/README.md`](zephyr-ws/klausscpu-zephyr/README.md).
The `apps/ssh_shell` app produces `zephyr.elf`, loaded to the FPGA with `klausscc`.

## Layout

| Path | Contents |
|---|---|
| `Makefile`, `programs/`, `src/`, `klausscpu.ld` | bare-metal runtime + test programs |
| `freertos/` | FreeRTOS port, lwIP glue, wolfSSL/wolfSSH, demos |
| `fatfs/` | FatFs glue for the **FreeRTOS** build (tracked) |
| `lwip_port/` | lwIP `sys_arch`/`ethernetif`/`cc.h` for KlaussCPU |
| `zephyr-ws/klausscpu-zephyr/` | Zephyr arch/SOC/board/drivers + `ssh_shell` app |
| `build-*.sh`, `get-*.sh` | upstream fetch/build scripts |

## Gotchas

- `picolibc` uses 64-bit `long`, so printf is `%lu`, not `%llu` (no `ll` modifier).
- lwIP checksum: `LWIP_CHKSUM_ALGORITHM=1` in `lwipopts.h` (LDIDX16 reads BE).
- The top-level `fatfs/` is the **FreeRTOS** FatFs glue (tracked) — *not* the
  west-managed `zephyr-ws/modules/fs/fatfs` (untracked/ignored). Don't confuse them.

## Relationship to `klausscpu-llvm`

```
klausscpu-llvm  (the fork)   →  builds the toolchain  →  build/bin/{clang,ld.lld,...}
        │                                                        │
        └──────────────── KLAUSSCPU_LLVM_BIN ───────────────────┘
                                  ↓
klausscpu-runtime (this repo) →  builds the software against it
```

A change spanning both repos (e.g. a new backend intrinsic + its use here) is two
commits in two repos — rare, since the backend is stable, but coordinate it.

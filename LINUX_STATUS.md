# Linux on KlaussCPU: where it stands (2026-11-13)

A hand-off for a new session. It says what runs on the board today, how to
build, install and check it, the rules for sharing the board, and what is
open. `LINUX_PLAN.md` holds the long history and every measurement, and
`DYNLINK_PLAN.md` covers shared libraries. Both are referred to below by
section name.

## The system in one paragraph

The board is a Nexys A7 running klaussCPU at 100 MHz (32-entry TLB, 16 KiB
I-cache, 64 KiB D-cache). It runs Linux 7.3.3 with 121 MB of usable RAM and a
256 MB swap file on the SD card. The kernel and initramfs are loaded over the
UART. The initramfs's `sdinit` mounts the SD card's system image read-only at
`/rofs`, puts a writable overlay over it as `/`, and mounts a persistent data
image at `/data`. The system image is Buildroot 2026.02.3 with shared
libraries:
- Python 3.14 with ctypes, OpenSSL 3.6, OpenSSH 10.2 on port 22, Dropbear on
  2222, sudo, `mc`, vim, htop, tmux, git, sqlite, curl, lua and the rest of
  `linux/buildroot/external/configs/klausscpu_defconfig`;
- busybox for the base system;
- musl's dynamic loader.

Logins are by SSH key only. bash is the login shell.

## Repositories (all on `main`/`master`, pushed)

| Repo | Head | What it holds |
|---|---|---|
| `klausscpu-runtime` (this) | 482bbde | kernel arch port (`linux/arch/klausscpu`), musl patches, initramfs, `sdinit`, Buildroot setup, image scripts, tests, validators, the plans |
| `klausscpu-llvm` | 220ef830 | the toolchain: clang, lld, compiler-rt; `build/bin` is what everything uses |
| `klausscc` | 19ff3a4 | the emulator, assembler and board loader; `target/release/klausscc` |
| `klaussCPU` | 166885e (`tlb32`, also master) | the RTL, owned by the CPU session |

Checkouts live side by side in `~/Documents/src` on the Beelink PC. The
scripts find each other relative to that.

## Hardware state

- **Flash:** the QSPI flash holds klaussCPU master 87ac3bb (l2_b3, written
  2026-11-16), and power-up boots it. `CPU_FEATURES` reads 0x07F0_200D:
  32 TLB entries, LR/SC, DCBZ, misaligned 64-bit accesses trap (emulated by
  the kernel), the hardware TLB walker (bit 25, MMU_PLAN §11.8) and the L2
  (bit 26).
  - **The L2:** 128 KiB, 4-way, 64 B lines with two 32 B sectors, a 3-cycle
    hit (HW_PERF_PLAN §6). It is coherent for every DDR master.
    `/proc/klausscpu_l2` reads 0 or 1 and takes 0 or 1 to switch it. Its
    counters are the `l2_*` lines in `/proc/klausscpu_perf`, and
    `/proc/cpuinfo` shows its geometry.
  - **Its effect on clang**, measured by the RTL session (L2 on against off,
    process time): deflate −16.5%, C++ −18.2%.
  - **The data TLB lookup** sits in the first EX cycle (§11.9).
  - **The I-cache** is 64 KiB direct-mapped with 32 B lines (HW_PERF_PLAN
    §5; `ICACHE_INFO` at 0xF005_0018, shown in `/proc/cpuinfo`). It has a
    4-entry micro-ITLB, tagged by main-TLB index.
  - **The counters:** `ic_miss` counts real line fills only, and
    `itlb_refills` counts micro-ITLB refills.
  - **The walker** refills useg misses from the page tables without an
    exception. The kernel (runtime dc07cb5) turns it on;
    `/proc/klausscpu_tlbwalk` shows it and takes 0 or 1, and `notlbwalk` on
    the command line keeps it off.
  - **Rollback:** the RTL session's `~/.cache/kbuild/rollback/README.txt`
    (the pre-L2 dump or the btic-fix bitstream 079d686, and the older
    images).
- **The emulator matches it:** klausscc defaults to 32 TLB entries and the
  walker. Whole-instruction restart after a walk is as in the RTL (klausscc
  cc309a6). `--tlb-entries 16` and `--no-tlb-walker` model older
  bitstreams.

## Board access and the board protocol (important)

- **Sharing:** the board is shared with the klaussCPU session (named
  `klausscpu-*`; `ListAgents` shows the current name).
- **Before using the board:** send it "board: taking it for X", and when done
  send "board: released, loaded = <bitstream>".
- **Locks:** run every board command under both locks:
  `flock /run/user/1000/klausscpu-board.lock flock ~/.cache/klausscpu-board.lock <cmd>`.
  `tools/validate/board-check.sh` takes them itself; `push.sh` and the test
  `board.sh` scripts do not.
- **Never:**
  - write the QSPI flash (the CPU session does that, with Graham's approval);
  - load a bitstream without the CPU session's agreement;
  - repartition the SD card or write outside `/linux/` on its FAT partition.
- **Processes:** kill only your own, by PID (never `pkill -f`).
- **SSH:** key-only, as root, at 192.168.68.59.
  - The scripts' key is `~/.ssh/klausscpu_board_ed25519` on the Beelink; it
    has no passphrase. `BOARD_SSH_KEY` overrides it.
  - Graham's own `~/.ssh/id_ed25519` is authorised for root as well.
  - The board's host key is pinned in `~/.cache/klausscpu-board-known_hosts`.
  - Fingerprint `SHA256:DYRxGyaKM2ZNhrjasuYI3VHyznQ7oWfC9LxA2KwsIYI`: Zephyr's
    key from the QSPI keystore, the same on both ports.
  - Quick shell:
    `ssh -i ~/.ssh/klausscpu_board_ed25519 -o IdentitiesOnly=yes root@192.168.68.59`.
- **Root's password is `*`.** Key logins work, `su` to root is impossible, and
  sudo uses each user's own password.
  - Never `passwd -l root`: its `!` makes OpenSSH refuse root's keys, which
    breaks all tooling. Dropbear on 2222 is the way back in if that happens.
- **Users:**
  - `graham` (uid 1000, in `wheel`, bash, Graham's Mac key, password set,
    sudo works).
  - New users: `adduser-ssh NAME 'KEY' [-G wheel]`.
- **Recovery:**
  - the UART console (a root shell; it doesn't ask for a password);
  - the initramfs fallback system (no SD image: Dropbear with the password);
  - `/linux/overlay.off` on the card disables the overlay;
  - `/linux/rootfs-static.img` is the old static system image.

## The SD card (`/linux/` on its FAT partition)

| File | What |
|---|---|
| `rootfs.img` | the system image (ext4, 256 MiB, read-only at `/rofs`), built from runtime a1625fb |
| `rootfs.img.next` | an image staged by `install-rootfs` while the old one was in use; sdinit swaps it in at boot |
| `data.img` | `/data` (224 MiB, read-write): `/root`, `/home`, `/data/etc` (users, passwords, settings), `/data/overlay` |
| `swap.img` | 256 MiB swap, turned on by rcS |
| `rootfs-static.img` | the previous, statically linked system, kept for going back |
| `toolchain.img` | clang, lld and the sysroot (ext4, 156 MiB, read-only at `/opt/llvm`; `install-toolchain`, `linux/toolchain/build-toolchain-image.sh`) |
| `toolchain.img.next` | a toolchain image staged by `install-toolchain` while `/opt/llvm` was mounted; rcS swaps it in at boot |

## Kernels

- **`linux-ws/build-board`:** the board kernel. It's build-perf's config plus
  `CONFIG_OVERLAY_FS`, with the busybox initramfs and the overlay `sdinit`.
  `board-check.sh --boot` loads it.
- **`linux-ws/build-perf`:** the CPU session's kernel, without overlayfs. With
  it the root is the plain read-only image, and everything else works.
- **Build:**
  `INITRAMFS_PROFILE=busybox LINUX_BUILD=$PWD/linux-ws/build-board linux/build.sh vmlinux`.
  The defconfig is `linux/arch/klausscpu/configs/defconfig`.
- **Load:** `klausscc -i <vmlinux> -s -m` loads it over the UART and keeps the
  console. `board-check.sh --boot` does this and logs the console.

## Building and changing the system

| Task | How |
|---|---|
| Build the Buildroot packages | `linux/buildroot/build.sh` (shared libraries, `linux-ws/br-output-dyn`; `BR_SHARED=0` gives the old static build) |
| Build the system image | `ROOTFS_BUILDROOT=1 linux/rootfs/build-rootfs.sh linux-ws/rootfs.img 256` |
| Install it (about 6.5 min, then a reboot) | `gzip -1c linux-ws/rootfs.img \| ssh root@board install-rootfs -z -`, then `tools/validate/board-check.sh --boot` |
| Add or update a package without an image (about 30 s) | `linux/buildroot/push.sh [-n] [-p] PKG`: builds it, rebuilds packages whose options it changes, and streams the files onto the overlay; `-p` adds it to the defconfig once it worked |
| Put a hand-made file on the board | `scp` it: it lands in the overlay. To keep it, also put it in the repo (`linux/userland`, `linux/initramfs/etc`, `build-rootfs.sh`) |
| See or clear the overlay | `overlay status`, `overlay files`, `overlay reset` (at the next boot) |
| Check the whole board | `tools/validate/board-check.sh [--boot] [--quick]`: 16 checks, about 8 min, takes both locks |
| Emulator tests | `tools/validate/linux.sh <out> <mode>` (modes in its header); `linux/userland/tests/{dynlink,libffi,cxx}/run.sh` |
| Build the C++ runtime (libunwind, libc++abi, libc++) | `linux/cxx/build.sh` (installs into `linux-ws/dyn/sysroot`) |
| Build clang for the board | `NATIVE_MULTICALL=1 NATIVE_BUILD_TYPE=Release NATIVE_PROFDATA=default NATIVE_LTO=Thin NATIVE_ORDER=1 NATIVE_BUILD=../klausscpu-llvm/build-native-klausscpu-pgo-lto linux/toolchain/build-native-llvm.sh` (PGO + ThinLTO + call-graph order; about 8 min; the profile is `linux/toolchain/clang.profdata.xz`, remade by `linux/toolchain/pgo-profile.sh`) |
| Build and install the toolchain image (about 5 min, no reboot) | `linux/toolchain/build-toolchain-image.sh`, then on the board `umount /opt/llvm`, then `gzip -1c linux-ws/toolchain.img \| ssh root@board install-toolchain -z -` (while `/opt/llvm` is mounted it is staged for the next boot instead) |
| Check the board's compiler | `linux/userland/tests/native-clang/board.sh` (identity with the cross clang, links, runs; `NC_SKIP=cxx-stdlib` saves an hour), `perf.sh` (timings under perfstat) |

- **A new image clears the overlay:** installing one moves the overlay's
  changes to `/data/overlay/upper.old` at boot, so pushed packages must be in
  the defconfig (`push.sh -p`) before the next image, or they're gone.
- **Buildroot never removes a deselected package's files** from
  `br-output-dyn/target`. Clean them out by hand (literal paths: the tool's
  `rm` safety check blocks paths built from variables).

## Toolchain facts that matter for Linux work

- **ABI:** LP64 with ELF32 files, 64 KiB pages. `ABI.md` in
  `klausscpu-llvm/llvm/lib/Target/KlaussCPU` is the reference.
- **Driver defaults:** static linking unless `-pie`/`-shared`. Graham chose
  not to make PIE the default; there's no ASLR.
- **Atomics:** RMW and cmpxchg are `__sync_*_N` calls (musl implements them
  with LR/SC). Since llvm 220ef830 clang defines
  `__GCC_HAVE_SYNC_COMPARE_AND_SWAP_1/2/4/8`.
- **Buildroot's compiler wrapper** (`linux/buildroot/toolchain.sh`) drops
  link-only options when only compiling, so meson works.
- **C++:** libc++, libc++abi and libunwind (KlaussCPU port) exist for the
  Linux triple, static and shared (`linux/cxx/build.sh`); `clang++ x.cpp`
  links them. Exceptions use R0/R1 at landing pads (ABI.md "Exceptions").
  Buildroot has C++ on, and the system image carries the shared libraries.
- **Code written at run time (JITs):** code written and run at the same
  address needs no maintenance; the I-cache snoops stores. Code written
  through one mapping and run through another (W^X double mapping) needs
  `__builtin___clear_cache`. compiler-rt's `__clear_cache` makes the
  `cacheflush(addr, bytes, ICACHE)` system call (244, as on csky/nios2;
  `arch/klausscpu/mm/cacheflush.c`), which runs ICACHE_INV. The architecture
  requires it, though today's core showed no stale case on the board
  (`linux/userland/tests/icache/aliasjit.c`). libffi's double-mapped closures
  call it.

## Compiling on the board

The board compiles and links its own programs with `/opt/llvm/bin/clang`
(on PATH in login shells). The objects and links are byte-identical to the
cross clang's (`CLANG_ON_BOARD_PLAN.md` M5).

```
clang -O2 hello.c -o hello            # static, like the cross driver
clang -O2 -fPIE -pie hello.c -o hello # dynamic, against /lib/libc.so
clang++ -O2 prog.cpp -o prog          # libc++, libc++abi, libunwind
clang -O2 -fPIC -shared lib.c -o libx.so
```

- **Where things are:** the headers and libraries are in
  `/opt/llvm/klausscpu-linux-musl/{include,lib}`, the driver's default
  sysroot, so no `--sysroot` is needed. clang's own headers are in
  `/opt/llvm/lib/clang/23/include`.
- **Libraries of your own** go anywhere, given `-I`/`-L`. Install shared ones
  where the loader looks (`/lib`, `/usr/lib`, the overlay keeps them), or
  use `-Wl,-rpath`.
- **The tools:** `ld.lld`, `llvm-ar`, `llvm-nm`, `llvm-objdump` and
  `llvm-readelf` are in `/opt/llvm/bin` too.
- **What to expect** (the PGO + ThinLTO clang of 2026-11-17, quiet board,
  `perf.sh`, user + sys):

  | compile | time |
  |---|---|
  | C hello, -O2 | about 1 s |
  | zlib deflate.c (2000 lines), preprocessed, -O0 / -O2 | 10 / 30 s |
  | the C++ hello with `<string>` and `<vector>`, preprocessed, -O0 / -O2 | 76 / 78 s (header parsing) |

  Older figures, taken before the hardware TLB walker, I-cache, L2 and the
  PGO build (so now about 3x faster): all of zlib (16 files at -O2, link,
  run the test) 7.5 min; the C++ hello from source 3.5 min, 44 s with a
  precompiled header.

- **Use -O0 or -O1 while developing:** codegen is most of the C time.
- **For C++, use a precompiled header.** `clang-pch` builds one of the
  common libc++ headers once per option set, into `~/.cache/clang-pch`, and
  prints the flag that uses it. Pass it the options that change what the
  headers mean: -O level, -std, -D, and -f flags such as -fno-exceptions.

  ```
  clang++ -O2 $(clang-pch -O2) prog.cpp -o prog
  ```

  The first use of an option set takes about 6 minutes and writes 14 MB.
  After that, the C++ hello compiles and links in about 53 s instead of
  3.5 minutes. `clang-pch [options] -- header...` takes your own header
  list. clang refuses a PCH built with different options and names the
  difference.

- **Memory:** a compile peaks at 50–80 MB. A big C++ file that includes
  much of libc++ swaps and takes over an hour (libc++'s own test
  `stdlib.cpp`: 80 min). Keep C++ translation units small.
- **Fixed cost:** a trivial C compile is about 19 M instructions, of which
  about 13 M is fixed per-process start-up: page faults, ~1800 option
  registrations, malloc. A link starts `ld.lld` as a second process.
  Fault-around of 16 pages (kernel 0876dbb) cut 5–11% of it.
- **Why it's slow:** clang's code (94 MB) is far bigger than the I-cache,
  the L2 and the TLB's reach: CPI is 3.3 (C) to 5.1 (C++), most of it
  instruction fetch and D-cache misses. M6 of `CLANG_ON_BOARD_PLAN.md`
  took deflate.c -O2 from 109 s to 30 s (hardware TLB walker, I-cache, L2,
  a call-graph function order, PGO + ThinLTO); that work is finished.

## Performance, where the time goes

For details see `LINUX_PLAN.md`: "TLB size and refill" and "Where Python's
time goes".
- **Program start:** a static one takes 18 ms, a dynamic one 33 ms, plus about
  15 ms per library.
- **Logins:** OpenSSH about 3.7 s, Dropbear 1.5 s.
- **Python:** after the 32-entry TLB, the biggest costs are:
  - the translation check cycle on every load and store (10–14% of cycles;
    hardware);
  - I-cache misses (a 32 KiB 2-way cache would save 12–17%);
  - the 1-entry micro-ITLB (2–3%).
- **Tools:**
  - klausscc `KLAUSSCC_TLBSIM=1` and `KLAUSSCC_CACHESIM=1` model other TLB
    and cache shapes;
  - `KLAUSSCC_PROFILE` gives per-PC profiles (`klausscc/tools/cacheprof`);
  - `perfstat` on the board gives the hardware counters, with exclusive stall
    attribution.

## Open items (none in progress)

- **Image installs:** the image could shrink to its contents (141 of 256 MiB
  used) and use `install-rootfs`'s HTTP download, which avoids the 0.6 MB/s
  SSH.
- **libpython:** build it with `-fno-semantic-interposition`. It calls itself
  through its PLT, about 4% of the Python loop's instructions.
- **sqlite's 64-bit multiply-overflow check** compiles to a 66-cycle DIVU
  (LLVM).
- **Bare-metal board baselines** (`tools/validate/baselines/board-O2`) are
  probably stale (tlb32, cache-info registers); refreshing them needs a board
  run.
- **OpenSSH start-up per process:** 0.86 s. Mostly OpenSSL 3 initialisation,
  P-256 and key shielding (`LINUX_PLAN.md`).
- **`linux-ws/br-output`, the static Buildroot build,** has only OpenSSL and
  OpenSSH rebuilt with the fast P-256 code.
- **musl `cacheflush()`:** add `#define __NR_cacheflush 244` to
  `linux/musl/arch/klausscpu/bits/syscall.h.in` at the next system-image
  rebuild; that gives `<sys/cachectl.h>`'s `cacheflush()`. It is held back
  because a rebuilt libc.so must match the board's `/lib/libc.so`. Until
  then use `__builtin___clear_cache` or `syscall(244, ...)`.
- **Branch `gcm-fuse-driver`** in this repo is not ours: crypto bitstream 2,
  untested on hardware. Leave it.

## Working conventions

- **Commits:** end every message with
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Push to `main`.
- **llvm main:** fixes and non-breaking improvements may be pushed there
  (Graham's standing permission). The LLVM session (`klausscpu-llvm-*`) owns
  backend design; tell it about changes.
- **Subagents:** Opus for correctness-critical code and reviews, Sonnet for
  builds, measurements and documentation (`~/.claude/CLAUDE.md`).
- **`/tmp` has a small quota:** use `~/.cache` or `linux-ws/` for scratch.
- **New work:** don't start tangential work without asking Graham.

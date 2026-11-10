# Linux on KlaussCPU — plan

**Status (2026-11-10): milestones 0–5 are done.** Linux runs busybox
init and ash on the emulator and on the board (klaussCPU `mmu` d9cf6c2,
the stage 3 release; `tools/validate/linux.sh <out> busybox board`).
- **Kernel:**
  - ASIDs, LR/SC atomics, cmpxchg and futex ops;
  - GENERIC_BUG, stack traces with KALLSYMS, and the exception table
    sorted at build time;
  - `hvc0` console, the SD card as `mmcblk0` (vfat), LiteEth as `eth0`,
    and the TRNG as `hwrng`;
  - SD transfers ported from Zephyr: a multi-block command per request
    (CMD18/CMD25, up to 64 KiB) and pipelined byte loops; `sd_spi.c`
    carries the protocol, host-tested by `linux/tools/sd_spi_test/`;
  - the native 4-bit SD controller as an mmc host (`sd_native.c`) where
    the bitstream has it, with interrupt source 2 as an Ethernet+SD
    cascade. It passes on the klausscc SD model (`linux.sh <out>
    sd-native`, and `sd-spi` for SPI) and awaits an `mmu-sd` bitstream;
  - the QSPI flash as read-only MTD (`qspi_flash.c`): its opcode check
    allows READ and RDID only, and partitions aren't writable;
  - ext4 and loop devices, for the root file system on the SD card.
  - the VGA port as `/dev/fb0` (`klausscpu_fb.c`), with DOOM on it
    (`linux/userland/doom.sh`); emulator-tested with
    `klausscpu_fb.force=1`, awaiting a board check (see "VGA frame
    buffer and DOOM").
- **User space:**
  - musl 1.2.5 (`linux/musl/`), threads included;
  - busybox (init with `/etc/init.d/rcS` and mdev; ash);
  - Dropbear 2026.94 (`linux/userland/dropbear.sh`): an SSH login over
    loopback passes on the emulator (`linux.sh <out> ssh`). It presents
    Zephyr's `ssh_shell` host key, read from the QSPI keystore (`kskey`,
    `linux.sh <out> keystore`);
  - `/init` is `sdinit` (`linux/userland/sdinit.c`): it boots the SD
    card's read-only system image `/linux/rootfs.img`, with persistent
    data in `/linux/data.img` at `/data` (`/root`, `/home`, `/data/etc`),
    and falls back to the initramfs (`linux.sh <out> rootfs`, `sd-spi`,
    `sd-native`; see "The SD card: system image and data image");
- **Board-checked (2026-11-10, d9cf6c2):**
  - the richer busybox config is built (vi without regex search; built
    with `-DBB_GLOBAL_CONST=`, see below);
  - SD card: `mmcblk0` (60 GB card) mounts as vfat;
  - Ethernet: DHCP from the router (192.168.68.59/22, route, DNS); from
    the PC, ping, busybox httpd and an SSH login to Dropbear all work
    (`tools/validate/linux.sh <out> net board`).
- **Bugs found on the board:**
  - Console input was lost. The UART has no RX interrupt and a 16-byte
    FIFO, and HVC's khvcd polling backs off to 2 s when idle. The timer
    tick now drains the FIFO into a ring and kicks khvcd (`hvc_uart.c`).
  - busybox's `*const` global pointers: clang hoists the load of a const
    global above its assignment (httpd stored to NULL+0x18).
    `CONFIG_EXTRA_CFLAGS="-DBB_GLOBAL_CONST="`.
  - `allnoconfig` left `CONFIG_UDHCPC_DEFAULT_SCRIPT` empty, so leases
    were never applied. rcS now also waits for the carrier and checks
    the address it ends up with.
- **Emulator:** SD card, Ethernet and TRNG aren't modelled there, and
  each driver's probe finds nothing.

### Board checks (networking, SSH, SD card)

Build with the busybox profile, which picks up Dropbear when
`linux/userland/dropbear.sh` has built it:

```
linux/musl/build.sh && linux/userland/busybox.sh && linux/userland/dropbear.sh
INITRAMFS_PROFILE=busybox linux/build.sh defconfig vmlinux
klausscc -i linux-ws/build/vmlinux -s -m
```

On the console, rcS:
- prints `eth0`'s address: from DHCP, which reserves 192.168.68.59 for
  MAC 00:ab:cd:00:00:01, or else 192.168.68.59/24 statically with gateway
  .1;
- starts `dropbear` on 22 and `httpd` on 80.

The dmesg lines to look for: `klausscpu-eth: ... LiteEth ..., PHY BMSR`,
`eth0: link up`, `klausscpu-sd: mmcblk0, N MiB`.

From the PC:
- `ping 192.168.68.59`
- `wget -O- http://192.168.68.59/` (the "Linux on KlaussCPU" page)
- `ssh root@192.168.68.59`, password `klausscpu`. The host key is
  generated on the first connection, which takes several seconds.

On the board:
- `ping -c 3 192.168.68.1`, `wget -O /tmp/x http://<PC>/file`
- `mount -t vfat /dev/mmcblk0p1 /mnt` (after `mkdir /mnt`), then `ls /mnt`
- `cat /proc/interrupts` (the `eth0` count rises with traffic)
- `dmesg | grep -i 'trng\|random'`

### Board checks: Zephyr's SSH host key (QSPI keystore)

Zephyr's `ssh_shell` keeps its ECDSA P-256 host key in NVS on the flash's
`storage` partition (0xF00000, 768 KiB), entry `ssh_host_key.bin` (SEC1
DER). Linux only reads the flash.
- `qspi_flash.c` refuses every opcode except READ (0x03) and RDID (0x9F).
- The MTD is `MTD_ROM`, and its partitions are registered without
  `MTD_WRITEABLE`.
- With no `qspi_ctrl` (the emulator, older bitstreams) the probe logs
  `klausscpu-qspi: no qspi_ctrl in this bitstream` and registers nothing.

`/etc/init.d/hostkey` picks the key for rcS's `dropbear`, in this order:
1. `kskey /dev/mtdN` (the `"storage"` line in `/proc/mtd`), converted
   with `dropbearconvert`;
2. else `/linux/dropbear_ed25519_host_key` on the SD card, generated once;
3. else `-R`.

Host tests: `linux/userland/keystore/test_kskey.sh` (synthetic NVS images:
newest-wins, torn write, sector rollover, wrap, deleted, bad CRC; plus an
`ssh-keygen` fingerprint round trip).

On the board:
- dmesg shows `klausscpu-qspi: S25FL128S 16 MiB, read-only`, and
  `cat /proc/mtd` lists `bitstream`, `storage` and `scratch` (mtd0–2).
- rcS prints `hostkey: Zephyr's key from the QSPI keystore (mtd1)`.
- `kskey /dev/mtd1 | head -1` prints `-----BEGIN EC PRIVATE KEY-----`.

From the PC, the fingerprint must match Zephyr's
(`SHA256:DYRxGyaKM2ZNhrjasuYI3VHyznQ7oWfC9LxA2KwsIYI`, ECDSA 256):

```
ssh-keyscan -t ecdsa 192.168.68.59 2>/dev/null | ssh-keygen -lf -
```

`ssh root@192.168.68.59` then logs in without a host-key warning, because
`known_hosts` already holds Zephyr's key.

### The SD card: system image and data image

The card keeps its single FAT partition and its files. Linux uses only
the `/linux/` folder:

| File | What | Written by |
|---|---|---|
| `rootfs.img` | the **system**: ext4, no journal, mounted **read-only** at `/` | `install-rootfs`, replaced whole |
| `data.img` | the **data**: ext4 with a journal, mounted read-write at `/data` | `install-data`, once |
| `rootfs.img.new`, `data.img.new` | an install in progress | renamed when complete |
| `rootfs.img.next`, `data.img.next` | an install staged while the old image was in use; `sdinit` swaps it in at the next boot | `install-rootfs`; `install-data --force` |
| `dropbear_ed25519_host_key` | the SSH host key, only when the QSPI key isn't found | `hostkey` |

Run-time layout of the system image:
- `/root` → `/data/root` and `/home` → `/data/home` (symlinks).
- `/data/etc` holds local settings. Examples are in
  `/data/etc/*.example`.
  - `network` holds variable assignments: `HOSTNAME`, and `IP`, `NETMASK`,
    `GATEWAY`, `DNS` for a static address instead of DHCP. rcS reads it
    in a subshell that hands back only those variables. A broken file
    gets `rcS: WARNING: /data/etc/network failed to load; using the
    defaults`, and the boot goes on. A static address that `ifconfig`
    rejects falls back to DHCP.
  - `rc.local` is run at the end of rcS, in its own shell, in the
    background, so it can't stop or hang the boot.
- The system `/etc` stays as built, so `passwd` doesn't work there.
- `/tmp` and `/run` are tmpfs.
- `/var/{run,log,lock}` → `/run/...`, and `/var/tmp` → `/tmp`.
- `/etc/resolv.conf` → `/run/resolv.conf`, `/etc/dropbear` →
  `/run/dropbear` (the QSPI host key is converted there at each boot), and
  `/etc/mtab` → `/proc/mounts`.

`data.img`'s cluster chain on FAT never changes after the install, so
writes inside it don't touch the FAT. The ext4 journal protects it across
resets. Errors remount it read-only.

The boot path (`sdinit`, the initramfs's `/init`):
- It mounts `mmcblk0p1` (vfat), swaps in any staged `.next` image, and
  loop-mounts `rootfs.img` read-only. An older image whose journal needs
  recovery mounts with `noload`.
- It loop-mounts `data.img` read-write on `/data`; the ext4 journal
  replays at mount, and nothing ever runs fsck on it.
- It switches root; the card stays at `/mnt`.
- **No `data.img`, or it doesn't mount:** `/data` becomes a tmpfs with
  `root/`, `home/` and `etc/`. `sdinit` says why
  (`sdinit: WARNING: no /mnt/sd/linux/data.img`, or `... does not mount
  as ext4 (left as it is)`), and rcS warns `/data is in RAM`.
- **The system image fails** (no card, no image, a mount error, or no
  `/sbin/init`): it falls back to the initramfs's busybox, as before.

First-time setup (PC, then board):

```
# On the PC: build both images
linux/rootfs/build-rootfs.sh linux-ws/rootfs.img 96
linux/rootfs/build-data.sh linux-ws/data.img 256     # size in MiB
# Board booted from the initramfs or an older SD root, network up:
gzip -1c linux-ws/data.img   | ssh root@192.168.68.59 install-data -z -
gzip -1c linux-ws/rootfs.img | ssh root@192.168.68.59 install-rootfs -z -
# reload the kernel: klausscc -i linux-ws/build/vmlinux -s -m
```

`install-data` refuses to replace an existing `data.img`
(`exists and holds user data: not replaced`), and reads stdin only with
`-z`, so a cut transfer fails gunzip's check.

`--force` replaces `data.img`. While a loop device holds it, the
replacement is staged as `data.img.next`. `sdinit` then keeps the old
image as `data.img.old` (delete it once the new one is right). If the new
one doesn't mount, `sdinit` restores the old one and keeps the new one as
`data.img.bad`.

Both install scripts `sync` before renaming the new file into place.

Roll back the kernel and both images together. An older `sdinit` mounts
the new journal-less system image read-write and doesn't mount `/data`.
A newer `sdinit` mounts an older system image read-only, which has no
`/data` or `/run` links.

Rebuild and update the system:

```
gzip -1c linux-ws/rootfs.img | ssh root@192.168.68.59 install-rootfs -z -
```

From the running SD root, it is staged as `rootfs.img.next` and swapped in
at the next boot. `install-rootfs` never touches `data.img`. Without SSH,
`install-rootfs [-z] http://<PC>:8000/rootfs.img [sha256]` downloads it,
but Graham's PC firewall blocks that.

`-z` makes the install scripts unpack the image themselves, so gunzip's
CRC-32 and length check covers the whole transfer; a damaged or truncated
stream fails and keeps the old image. Times on the board for the 96 MiB
system image (1.1 MB gzipped):

| Path | Time |
|---|---|
| SPI, raw | 22 min |
| SPI, gzip + SHA-256 | 7 min 20 s |
| native SD, gzip + SHA-256 | 4 min |
| native SD, `-z` | 78 s |

Board checks:
- The console shows `sdinit: data.img mounted read-write at /data` and
  `sdinit: switching to the SD card's root file system (read-only)`.
- `grep -E ' / | /data ' /proc/mounts` shows `/dev/loop0 / ext4 ro` and
  `/dev/loop1 /data ext4 rw`.
- `touch /usr/x` fails (read-only file system), and `ls -l / | grep -E
  'root|home'` shows the links.
- `cat /etc/klausscpu-rootfs` prints the build date and runtime commit.
- **Persistence across a system update:**
  1. `echo x > /root/t; sync`;
  2. push a rebuilt `rootfs.img` with `install-rootfs -z -`;
  3. reload the kernel;
  4. the new `/etc/klausscpu-rootfs` is there, and `cat /root/t` gives `x`.
- **Settings:**
  1. `echo HOSTNAME=klaustest > /data/etc/network`, reload;
  2. rcS prints `settings from /data/etc/network`, and `hostname` is
     `klaustest`;
  3. remove the file again.
- **A static address:** set `IP=`, `NETMASK=`, `GATEWAY=` and `DNS=` there;
  rcS then prints `static address ...` instead of running DHCP.
- `install-data -z - < /dev/null` refuses (data.img exists).
- Run `sync` before resetting the board. The data image's journal covers
  a reset, but anything not yet synced is lost.

Emulator: `linux.sh sd-spi` and `linux.sh sd-native` run this whole cycle
on the klausscc SD model:
- install both images;
- show the second `install-data` refusing;
- show `/` read-only and `/data` read-write;
- write `/root/t`, install a newer system image B, reboot, and find
  `/root/t` and the `/data/etc` settings still there;
- boot with a broken `/data/etc/network` (warning, defaults, boot goes
  on);
- boot once without `data.img`.

Boot 3 also shows the journal at work: boot 2 ended without an unmount,
as a board reset does, and boot 3 logs `EXT4-fs (loop1): recovery
complete` when it mounts `data.img`.

### Ported programs

Each program has a script in `linux/userland/<pkg>.sh`. The script
fetches a pinned upstream release into `linux-ws/<pkg>`, checks its hash,
and builds a static musl binary in `linux-ws/<pkg>-build`.
`build-rootfs.sh` installs whatever has been built. The self-tests go to
`/usr/share/klausscpu/tests`, so they can be run on the board too.

`tools/validate/linux.sh <out> ports` builds any port that is missing.
It boots a 24 MiB system image and an 8 MiB data image through `sdinit`
from the initramfs, so `/data` is real ext4. It runs one boot per port.
`LINUX_PORTS="lua sqlite bench"` picks some of them.

| Port | Version | Installed | Self-test (emulator result) |
|---|---|---|---|
| Lua | 5.4.9 | `lua`, `luac` | `tests/lua-tests.sh`: 19 of Lua's own test files plus `lua-selftest.lua` (also as luac bytecode): 19/19, PASS |
| SQLite | 3.54.1 amalgamation | `sqlite3` | `tests/sqlite-test.sh`: a 64-line known-answer test on `/data/sqlite-test.db`, reopened by a second process: PASS at -O2, and also at -O3 and -Os |
| CoreMark | eembc 1f483d5 | `coremark` | in `tests/bench.sh quick`: the CRCs for the performance seeds |
| lmbench 3 (subset) | intel/lmbench 27b43ae | `/usr/libexec/lmbench/`: `lat_syscall`, `lat_proc`, `lat_ctx`, `lat_pagefault`, `bw_mem`, `lat_mem_rd`, `tlb` | `tests/bench.sh quick`: each prints a result |
| tlbmiss | ours (`linux/userland/tlbmiss.c`) | `tlbmiss` | ns per access through N pages, one page (64 KiB) apart |
| iperf3 | 3.22 | `iperf3` | `tests/iperf3-test.sh`: a TCP and a UDP run over loopback against one-shot servers: PASS |
| curl + mbedTLS | curl 8.23.0, mbedTLS 3.6.7 LTS | `curl`; `/etc/ssl/certs/ca-certificates.crt` (curl.se's Mozilla bundle, 2026-11-05); `/usr/libexec/mbedtls-selftest` | `tests/curl-test.sh`: file://, HTTP on loopback against busybox httpd (a page, HEAD, a 1 MiB download checked by SHA-256, a 404), the CA bundle, and mbedTLS's own self-test: 7/7 |

Notes:
- **SQLite and branch range.** It needs `-ffunction-sections`.
  `sqlite3.o`'s `.text` alone is beyond KlaussCPU's branch range, and lld
  places range-extension thunks only between input sections. Without the
  flag the link fails with `InputSection too large for range extension
  thunk`. Any very large single-file program will hit the same limit.
- **musl and locales.** musl's `setlocale` accepts any locale name, but
  keeps the C number format. `lua-tests.sh` therefore refuses non-C
  locales, as a C library without them does; otherwise `literals.lua`'s
  pt_BR block fails.
- **CoreMark's 10 s rule.** CoreMark counts any run shorter than 10 s as
  an error. The emulator's 20-iteration run is checked by its CRCs
  instead.
- **iperf3 and curl configure.** Both use autoconf, whose `config.sub`
  doesn't know `klausscpu`. They are configured with
  `--host=riscv64-unknown-linux-musl`, which only puts configure in cross
  mode: every test compiles with our clang. curl's `CURL_OS` is then
  corrected, so `curl --version` says `klausscpu-unknown-linux-musl`.
- **curl's TLS library.** curl 8.15 dropped BearSSL, so it uses mbedTLS.
  It has HTTP(S), FTP and FILE, and the synchronous resolver.
- **TLB on the emulator.** `tlbmiss` jumps between 8 and 16 pages, and
  `tlb` reports 15 pages: the 16-entry TLB, less the entries the code and
  stack use.

On the board, after `install-rootfs` with an image that has them:

```
/usr/share/klausscpu/tests/lua-tests.sh
/usr/share/klausscpu/tests/sqlite-test.sh            # uses /data
/usr/share/klausscpu/tests/bench.sh | tee /data/bench-$(date +%F).txt
/usr/share/klausscpu/tests/iperf3-test.sh     # loopback
/usr/share/klausscpu/tests/curl-test.sh
iperf3 -s                       # then on the PC: iperf3 -c 192.168.68.59 [-R]
curl -sS https://example.com/ | head    # https with the CA bundle
```

`bench.sh` without `quick`:
- CoreMark sizes its own run to at least 10 s. The score is the
  `Iterations/Sec` line, and `Correct operation validated` means it is a
  valid result.
- lmbench runs with its default repetitions: `bw_mem` 8 MB,
  `lat_mem_rd` up to 32 MB at stride 64, `lat_ctx` with 2–16
  processes.
- `tlbmiss` runs up to 64 pages.

### Buildroot packages

Packages are a Buildroot config switch rather than a hand port each.
Buildroot builds only the packages. `linux/build.sh` still builds the
kernel, and our initramfs, `sdinit`, `rcS` and layout stay.

**Files:**

| File | What |
|---|---|
| `linux/buildroot/fetch.sh` | Buildroot 2026.02.3 (LTS, SHA-256 pinned) into `linux-ws/buildroot`, with `linux/buildroot/patches/*` applied |
| `patches/0001-gnuconfig-klausscpu.patch` | `support/gnuconfig/config.sub` knows `klausscpu`. Buildroot copies it into every autotools package, so they configure as `--host=klausscpu-buildroot-linux-musl` |
| `patches/0002-arch-klausscpu.patch` | `BR2_klausscpu`: 64-bit little-endian, MMU, soft float, readelf name `KlaussCPU` (EM 0x4B43) |
| `patches/0003-htop-static.patch` | htop with `BR2_STATIC_LIBS`: htop's own `--enable-static`, without its dlopen() features |
| `patches/0004-strace-bundled-headers.patch` | strace on KlaussCPU uses its bundled uapi headers (our 7.3 headers are newer than strace 6.19 expects) |
| `linux/buildroot/toolchain.sh` | `linux-ws/br-toolchain`: gcc-named wrappers around our clang and the LLVM tools, and a usr/-layout copy of the musl sysroot with the 7.3 headers |
| `linux/buildroot/external/` | the BR2_EXTERNAL tree: `configs/klausscpu_defconfig` and `patches/strace/0001-klausscpu.patch` (strace's KlaussCPU port) |
| `linux/buildroot/build.sh [targets]` | build into `linux-ws/br-output` (downloads in `linux-ws/br-dl`); the result is `images/rootfs.tar` |

**The toolchain wrapper.** `klausscpu-linux-musl-gcc` runs clang
`--target=klausscpu-unknown-linux-musl`:
- It answers `-dumpversion` with 14.2.0, since the defconfig selects
  GCC 14.
- It drops `-msoft-float`.
- It links statically, with `-ffunction-sections` by default.
- For Buildroot's `--sysroot=<staging>` it adds `usr/include` with
  `-idirafter` and `usr/lib` last, after the package's own directories,
  as gcc's sysroot directories come. Both orders mattered:
  - with the sysroot `-L` first, ncurses linked staging's older
    `libncurses.a`;
  - with the sysroot includes first, strace saw the sysroot's
    `io_uring.h` instead of its bundled one.

**The defconfig:**
- external custom toolchain, musl, static libs;
- headers 6.19, Buildroot's latest, which makes the 7.3 check loose;
- no SSP, no init, no `/bin/sh`, no busybox;
- `rootfs.tar`;
- `BR2_GLOBAL_PATCH_DIR` pointing at the external `patches/`.

**Into the system image:**

```
linux/buildroot/build.sh                       # all packages
ROOTFS_BUILDROOT=1 linux/rootfs/build-rootfs.sh linux-ws/rootfs.img 96
```

This merges `bin`, `sbin` and `usr` from `rootfs.tar`:
- A package's program replaces busybox's applet of that name in every
  bin directory (`/bin/less` would otherwise shadow `/usr/bin/less`).
- It never replaces one of our own files.
- Buildroot's `/etc`, headers and static libraries stay out.

**Emulator check.** `tools/validate/linux.sh <out> buildroot` boots that
image, as the ports mode does, and runs `tests/buildroot-test.sh`. All
10/10 pass. `LINUX_BUILDROOT_PREBUILT=1` reuses `br-output`, and
`LINUX_BUILDROOT_EXPECT=N` requires at least N packages.

**Applets win in the shell.** The busybox shell is built with
`FEATURE_PREFER_APPLETS`, so typing `less` runs busybox's applet even
though `/usr/bin/less` is installed. The test therefore calls each
package by its real path. To make packages win interactively, drop the
overlapping applets from `busybox.config`, or turn
`FEATURE_PREFER_APPLETS` off; the system image has applet links, so it
doesn't need the setting.

| Package | Version | Check |
|---|---|---|
| zlib | 1.3.2 | `file -z` on a gzip stream |
| ncurses | 6.6 | `infocmp vt100` (cols#80), `tput clear` |
| nano | 8.7 | `--version` |
| file | 5.46 | `file` on an ELF and a script |
| less | 692 | the real less (not busybox's), `-F` on a pipe |
| tree | 2.2.1 | a small tree's summary |
| jq | 1.8.1 | `.a\|map(.*2)` |
| htop | 3.4.1 | `--version` |
| tmux | 3.6a (libevent 2.1.12) | a detached session, `ls`, `kill-server` |
| strace | 6.19 | see below |

**strace.**
- The kernel gained `PTRACE_GETREGSET`/`SETREGSET` (`NT_PRSTATUS`) and
  `PTRACE_PEEKUSR`/`POKEUSR`, over r0–r15, sp, pc and flags
  (`arch/klausscpu/kernel/ptrace.c`, `HAVE_ARCH_TRACEHOOK`).
- The uapi register struct is now `struct user_pt_regs`, as on arm64.
  It was `user_regs_struct`, which is the C library's name
  (`<sys/user.h>`) and clashed in programs that include both. The layout
  is unchanged.
- strace reads the syscall number and arguments with
  `PTRACE_GET_SYSCALL_INFO`, and changes them with `PTRACE_SET_SYSCALL_INFO`.
  At entry r12 already reads -ENOSYS, as on arm64.
- strace's `linux/klausscpu/` port uses the generic 64-bit syscall table,
  arguments r0–r5, and the result in r12.
- Checks:
  - `strace -f /bin/true` and `strace ls /`;
  - an ENOENT `openat`;
  - `-e inject=uname:error=EPERM` (`(INJECTED)`).
- strace 6.19 also needed:
  - an `arch_sigreturn` decoder, which prints the rt_sigframe's mask;
    with clang's -Wunused-function, strace's `-Werror` build fails on
    the generic empty one;
  - its bundled uapi headers (patch 0004).
- On the emulator, `strace /bin/echo hi` shows `execve`,
  `set_tid_address`, `brk`, `mmap(... MAP_PRIVATE|MAP_ANONYMOUS ...)`,
  `write(1, "hi\n", 3) = 3` and `exit_group(0)`.

### Board results (2026-11-10, release bitstream from QSPI)

Measured on the board (klaussCPU master 2743116, mmu-sd 5648f75 LEAN,
100 MHz) under Linux from the SD root, with the ported programs:

| What | Result |
|---|---|
| Lua 5.4 self-tests, SQLite known-answer test | PASS |
| CoreMark (-O2, 3000 iterations, 17.5 s) | 171.7 it/s (1.72 / MHz) |
| null syscall / read / write | 4.7 / 10.9 / 9.3 us |
| stat / open+close | 122 / 131 us |
| fork+exit / fork+execve / fork+sh -c | 42.6 / 78.4 / 143 ms |
| context switch, 2 / 16 processes | 101 / 203 us |
| page fault (file) | 77 us |
| memory bandwidth read / write / copy (8 MiB) | 47 / 37 / 21 MB/s |
| load latency: D-cache / DDR (lat_mem_rd, stride 64) | 42 ns / 617 ns |
| TLB miss + refill (tlbmiss, 64 KiB stride, >15 pages) | ~2.9 us (~290 cycles) |
| iperf3 board -> PC / PC -> board | 13.8 / 10.9 Mbit/s (PC -> board: 1360 retransmits, 94 RX drops) |
| curl https://example.com/ (mbedTLS, CA bundle) | 200, verified, 4.0 s |
| VGA vsync / DOOM timedemo demo1 | 59.52 Hz / 33.2 fps |

Tuning candidates the numbers point at:
- the TLB refill path (about 290 cycles per miss; 16 entries of 64 KiB);
- system-call and fork/exec overheads;
- Ethernet receive (the LiteEth has 2 RX slots, so line-rate traffic drops packets while the CPU drains them).

### Board checks: SD card speed

The driver (`sdcard.c` over `sd_spi.c`) sends each request, up to 64 KiB,
as one multi-block command:
- reads: CMD18, then CMD12;
- writes: ACMD23 and CMD25, with 0xFC block tokens and the 0xFD stop
  token, waiting for busy after each block.

The data loops are pipelined, as in Zephyr's driver: a read starts byte
i+1 before reading byte i. The clock stays at 25 MHz, and the init
sequence is unchanged.

A read that may have lost a byte is detected and redone with the plain
loop. A multi-block command that keeps failing is redone with one command
per sector, and logs `klausscpu-sd: multi-block ... retrying per block`.

Both switches are on by default, for A/B comparisons, and there are
counters beside them:

```
/sys/module/klausscpu_sd/parameters/{multiblock,pipeline}
/sys/module/klausscpu_sd/parameters/{late_reads,multi_retries,multi_fallbacks,retries}
```

Write 0 to a switch to turn it off. On the kernel command line, the
switches are `klausscpu_sd.multiblock=0` and `klausscpu_sd.pipeline=0`.
The boot line shows both:
`klausscpu-sd: mmcblk0, N MiB (SDHC/SDXC), multiblock 1, pipeline 1`.

Host test, no board needed:

```
linux/tools/sd_spi_test/run.sh [ops] [seed]
```

It runs `sd_spi.c` against a model of the controller and a card. The
model has a latched DATA register, stalls, CMD12, busy periods, and a
stop token that is ignored while busy. It injects faults: rejected
commands, error tokens, rejected writes.

The test checks every read against the expected data. After each
operation it also checks that the card's contents match, so no write
lands outside the request. Three deliberately broken versions of
`sd_spi.c` each fail it:
- no late check;
- no busy wait before the stop token;
- no CMD12.

The benchmark needs a busybox with `dd`: busybox.config now has it, so
rebuild with `linux/userland/busybox.sh`.

The benchmark reads only, except for one 16 MiB test file,
`/linux/bench`, which it writes through vfat and then deletes. Run it
from the SD root file system, where the card's partition is at `/mnt`.
From the initramfs, first run `mkdir -p /mnt; mount -t vfat
/dev/mmcblk0p1 /mnt`.

```
P=/sys/module/klausscpu_sd/parameters
bench() {
	echo "multiblock $(cat $P/multiblock) pipeline $(cat $P/pipeline)"
	sync; echo 3 > /proc/sys/vm/drop_caches
	time dd if=/dev/mmcblk0 of=/dev/null bs=64k count=256
	time sh -c 'dd if=/dev/zero of=/mnt/linux/bench bs=64k count=256 && sync'
	rm /mnt/linux/bench; sync
}
bench
echo 0 > $P/pipeline; bench
echo 0 > $P/multiblock; bench
echo 1 > $P/pipeline; bench
echo 1 > $P/multiblock
cat $P/late_reads $P/multi_fallbacks $P/retries
```

How to read the results:
- The read test reads raw blocks (16 MiB from the start of the card).
  `drop_caches` makes each run go to the card.
- For writes, `time` includes the `sync`. dd's own rate stops at the page
  cache.
- `late_reads` above zero means the pipelined read loop sometimes reads
  late. Each such block costs one plain retry.

### Native 4-bit SD controller (klaussCPU `mmu-sd`)

`sd_native.c` drives the controller in klaussCPU SD_NATIVE_PLAN.md
(register map v1, 951c62c) as an mmc host. The mmc core runs the card
protocol, and mmc_block names the card `mmcblk0`. That is the name the
SPI driver uses too, so `sdinit`, `install-rootfs` and `hostkey` work
with either driver.

Which driver runs:

| Bitstream | Driver |
|---|---|
| `CPU_FEATURES[21]` set and `SDM_CAPS` ≠ 0 | native; the SPI driver logs `the native SD controller has the card` and registers nothing |
| otherwise (older bitstreams, the emulator for now) | SPI, unchanged; the native driver finds nothing |

`klausscpu_sd.native=0` keeps SPI on a native bitstream for A/B
comparisons. The loader passes no command line, so add it to
`CONFIG_CMDLINE` and rebuild.

The interrupt cascade:
- On a native bitstream, interrupt source 2 is a chained irqchip with two
  children, masked through `INT2_EN` (0xF00F_0050). Child 0 is LiteEth,
  child 1 is SD.
- Boot logs `klausscpu-intc: source 2 cascades Ethernet and SD`, and
  `/proc/interrupts` lists both children under `klausscpu-int2`.
- Elsewhere, `INT2_*` read 0, so source 2 stays Ethernet's own line, as
  before.

The driver:
- **Power and clock.** At probe it selects SD mode with the slot powered
  off. `set_ios(POWER_UP)` waits until the slot has been off 20 ms
  (≥ 10 ms is needed; to be measured), then powers it.
  `set_ios(POWER_ON)` sets `CLK_ON`, so the clock runs freely during the
  core's `power_delay_ms` (≥ 74 clocks). The first request clears it.
- **Clock rates.** The clock runs at 400 kHz during identification, then
  25 MHz. High speed (50 MHz) waits for phase 3.
- **Bus.** 4-bit when `SDM_CAPS[8]` is set. It handles SD cards only
  (no SDIO or MMC probing).
- **Requests.** A threaded IRQ handler runs each request: CMD23 when the
  card supports it, then the command and its data, then CMD12.
  - CMD12 is always sent as R1b with `SDM_CMD[13]` set.
  - Data moves by PIO, one block per FIFO_THRESH, through `sg_miter`.
  - Errors map to -ETIMEDOUT (timeouts, no CRC token) and -EILSEQ
    (CRC, end bit, index, write CRC token).
  - `bytes_xfered` = `SDM_BLKDONE` × block size, after the good blocks of a
    failed read have been drained.
  - A watchdog fails a request that never completes, instead of hanging
    the mmc core.
- **Block sizes.** Any multiple of 4 from 4 to 2048 bytes (plan §4.3,
  57a5a8c). A block takes ceil(size/8) FIFO words; a 4-mod-8 block's last
  word carries its 4 bytes in [31:0]. The 4-byte case is ACMD22, which the
  core sends after a write error to learn how many blocks reached the
  card. Other sizes are refused with -EINVAL.
- **Test path.** `linux/userland/sdcmd.c` sends ACMD51 (8 bytes), ACMD13
  (64 bytes) and, after an 8-sector O_DIRECT write, ACMD22 (4 bytes)
  through mmc_block's `MMC_IOC_CMD`. `linux.sh sd-native` runs it.
  Its `wtest` writes raw sectors, so it ships only in that test's
  initramfs.
- **Card changes.** The driver doesn't poll for card changes: insert the
  card before boot.
- **Boot wait.** The mmc core finds the card asynchronously. When an mmc
  host exists, `sdinit` waits up to 3 s for `/dev/mmcblk0p1`.

The SPI driver now powers the slot off for 20 ms, in SPI mode
(`SD_CTRL[18]` = 0), before its first CMD0. This follows plan §4.1: a
load after native mode resets the controller, but only that power cycle
reliably resets a card left in any state.

Status:
- It builds W=1 clean. On bitstreams without the controller the probe
  finds nothing, and the `busybox`, `rootfs` and `ssh` modes pass.
- It runs on the klausscc SD model (5e73d28: `--sd-image FILE`, plus
  `--sd-native` for this controller): `tools/validate/linux.sh <out>
  sd-native`, and `sd-spi` for the SPI driver on the same card. The
  emulator must be the build with the SD model; set
  `KLAUSSCC=~/.cache/klausscc-mmu-bin/klausscc`.
- Each SD mode makes a 64 MiB card image in `<out>/card.img`: an MBR,
  with one FAT32 partition from sector 2048. It then boots twice.

  Boot 1 starts from the initramfs, since there is no image on the card
  yet:
  - `install-rootfs -` writes the 16 MiB rootfs image to `/linux/`
    through vfat;
  - after `drop_caches`, its SHA-256 matches;
  - in `sd-spi`, it also matches with multiblock and pipeline off, and a
    256 KiB file written with the switches off reads back the same;
  - after `umount`, the hash of the first 8 MiB of raw `/dev/mmcblk0`
    equals the image file's;
  - `fsck.fat -n` is clean.

  Boot 2: sdinit loop-mounts the card's `/linux/rootfs.img` as `/`. A
  file written there is synced, and musltest passes.
- Boot 1 takes 4.0 G emulated instructions with the native driver and
  6.1 G over SPI. The emulator doesn't model bus speed, so the board
  numbers are what count.

Fault injection (klausscc 293be13 `--sd-fault`, transient faults): both
SD modes then boot once per fault on a fresh card. Each boot writes a
2 MiB file through vfat and reads it back after `drop_caches`, then
checks:
- that the fault was injected;
- the file's SHA-256;
- `fsck.fat`;
- that a counter shows the error path was taken.

Both modes pass every case:

| Fault | SPI driver | Native driver and mmc core |
|---|---|---|
| `wcrc@1000`: a block mid-CMD25 rejected | the multi-block write is retried (`multi_retries` 1) | e.g. `CMD25 …: data error -84 … after 929 of 2048 blocks`; the core asks ACMD22 for the count, and the rest is requeued |
| `wbusy@1000`: write busy never ends | 750 ms busy timeout, then a retry | `data error -110` (BUSY_TIMEOUT), CMD12, then ACMD22 and requeue |
| `wbusy@1`: busy on a single-block write (vfat's dirty flag) | retried (`retries` 1); before the fix this lost the write (`I/O error … lost sync page write`) | `CMD24 …: data error -110 … after 0 of 1 blocks`; the core recovers |
| `rerr@3000`: a bad block mid-CMD18 | the multi-block read is retried | `CMD18 …: data error -84 … after 15 of 256 blocks`; the core retries |
| `cmdto:18@8`: no response to CMD18 | retried | command error -110; the core retries (`native_cmd_errors` 1) |
| `cmdto:13@5`: no response to CMD13 (native only) | — | the core's status poll retries |

What Linux logs: the mmc core says nothing when its recovery works (it
prints only `recovery failed!`).
- The native driver adds a rate-limited `data error` line, and counts
  errors in `native_cmd_errors`, `native_data_errors` and
  `native_timeouts`.
- The SPI driver logs only when a request falls back to one command per
  sector, or fails. Its retries show in `multi_retries` and `retries`.
- In none of these cases did the watchdog fire, or a request fall back
  or fail.

An independent review of the driver against the plan found:
- ACMD22's 4-byte block (refused at first; supported since the
  controller gained 4-byte blocks);
- commands that could start while the engine was busy (now refused);
- a `jiffies` race in the power-up wait;
- a missing `MMC_BLOCK` dependency.

All four are fixed.

Board check, on a native bitstream:
- dmesg: `klausscpu-sdnative ...: native SD controller v1, 8-block FIFO,
  4-bit, IRQ N`, `mmc0: new SDHC card at address ...`, then
  `mmcblk0: mmc0:... p1`.
- `cat /sys/class/mmc_host/mmc0/mmc0:*/type` (SD) and
  `cat /sys/kernel/debug/mmc0/ios`, if debugfs is mounted: the clock and
  the 4-bit bus.
- The SD speed benchmark above. It reads `/dev/mmcblk0` and writes only
  `/linux/bench`; the `klausscpu_sd` switches apply to SPI only.
- Compare with SPI on the same bitstream: rebuild with
  `klausscpu_sd.native=0` in `CONFIG_CMDLINE`.

### VGA frame buffer and DOOM

`arch/klausscpu/kernel/klausscpu_fb.c` (`CONFIG_KLAUSSCPU_FB`, on with
`CONFIG_FB`) drives the VGA port as `/dev/fb0`. The controller is in every
`mmu` bitstream since the vga-output merge (8e9a477), so d9cf6c2 has it.

The hardware (klaussCPU MMIO_MAP.md, VGA_PLAN.md):
- a fixed 640x480 @ 59.5 Hz picture, scanned out of DDR: HEIGHT lines of
  640 pixels, or of 320 shown twice each way, STRIDE bytes apart from
  FB_BASE (32-byte aligned), starting at display line VSTART;
- RGB565 (the display uses the top 4 bits of each channel), or 8-bit
  indices into a 256-entry RGB444 palette;
- FB_BASE and the geometry latch at the start of vblank, so a pan is a
  tear-free flip. The palette changes at once;
- vsync is interrupt source 3;
- the scan-out reads DDR, past the CPU's write-back D-cache. The only
  flush is the whole cache (`CACHE_CTRL` FLUSH, a walk of a few thousand
  cycles).

The driver:
- **Memory.** 1200 KiB of ordinary kernel memory (two 640x480 RGB565
  pages), allocated at boot and mapped into user space by PFN. The
  display has no fixed address, so nothing is reserved.
- **Modes** (`FBIOPUT_VSCREENINFO`): xres 640, or 320 (doubled); 8 or
  16 bpp; any yres up to 480 (240 doubled), centred; `yres_virtual` as far
  as the buffer allows, for page flips with `FBIOPAN_DISPLAY`. The default
  is 640x480 RGB565. 8 bpp is `PSEUDOCOLOR`, and the palette is loaded
  with `FBIOPUTCMAP`.
- **Coherency.**
  - While `/dev/fb0` is open or mapped, the vsync interrupt flushes the
    cache at the start of each vblank, so what a program draws through
    the mapping shows within a frame.
  - `write()`, a pan, `FBIO_WAITFORVSYNC` and the last close or unmap
    also flush.
  - With nobody using it, the interrupt is off and nothing is flushed.
- **No controller** (the emulator, older bitstreams): BORDER doesn't
  read back, and the driver logs `klausscpu-fb: no VGA controller in this
  bitstream`. `klausscpu_fb.force=1` registers `/dev/fb0` anyway,
  without touching the display, to test programs on the emulator.
- `/sys/module/klausscpu_fb/parameters/status` shows the live frame
  counter, raster line, underflow count, vsync interrupts, flushes and
  users.

Programs:
- `fbtest` (`linux/userland/fbtest.c`, in the system image):
  - `check` sets every mode, writes through the mapping, reads it back
    with `read()`, pans, and waits for vsync;
  - `bars [WxHxB]` draws colour bars;
  - `shot` writes the visible page, and the palette, for
    `linux/tools/fb2png.py`;
  - also `info`, `fill`, `cmap` and `vsync [n]`.
- `linux/tools/fbshot.sh [root@IP] [out.png]` takes a screenshot over
  SSH. It writes the image at its own resolution, and `out-screen.png`, the
  640x480 picture as the monitor shows it.
- `doom`: doomgeneric (pinned e257a09, the Zephyr port's commit, with its
  64-bit timer patch) with an fbdev backend,
  `linux/userland/doom/doomgeneric_klausscpu.c`. `linux/userland/doom.sh`
  builds it (static, -O2, 570 KB). `build-rootfs.sh` installs it when it
  has been built.
  - It sets 320x200 at 8 bpp with three pages. Doom renders straight into
    a page that isn't on screen, then flips to it with a pan, and loads
    the palette when it changes.
  - Other frame buffers get converted 16 or 32 bpp frames.
  - On exit, or on Ctrl-C, it restores the mode.
  - The WAD: `-iwad FILE`, else `/mnt/DOOM1.WAD`, the SD card's root.
    None ships in the image.
  - Keys from the terminal (the UART or SSH): arrows, `z` or `,` to fire,
    space to open, Enter/Esc for menus, Tab for the map, 1-7 for weapons.
    A terminal sends no key releases, so a key counts as released when it
    hasn't repeated for 100-180 ms. Without a terminal, Doom plays its
    demos.
  - `DOOM_FPS=1` prints the frame rate every 10 s.

Emulator (no VGA model, `klausscpu_fb.force=1` in `CONFIG_CMDLINE`):
- `fbtest check` passes.
- `doom` runs the attract loop, and `doom -timedemo demo1` finishes (5026
  gametics).
- A mid-demo `fbtest shot`, converted by `fb2png.py`, is a correct Doom
  frame.
- Without `/dev/fb0`, `doom` exits 1 with `no frame buffer`. Without a
  WAD it says so and exits 1.

Board check (d9cf6c2 or later; the kernel's defconfig has `CONFIG_FB`):

```
linux/userland/doom.sh
INITRAMFS_PROFILE=busybox linux/build.sh defconfig vmlinux
linux/rootfs/build-rootfs.sh linux-ws/rootfs.img 96
gzip -1c linux-ws/rootfs.img | ssh root@192.168.68.59 install-rootfs -z -
klausscc -i linux-ws/build/vmlinux -s -m
```

On the board:
1. dmesg shows `fb0: klausscpu-vga 640x480 RGB565, 1200 KiB at ...,
   vsync IRQ`, and the monitor goes black.
2. `fbtest vsync 60` prints about 59.5 Hz, and `grep vga
   /proc/interrupts` shows the count.
3. `fbtest check` prints `fbtest: PASS`.
4. `cat /sys/module/klausscpu_fb/parameters/status` shows the frame count
   rising and `underflows 0`.
5. `fbtest bars` shows colour bars over a grey ramp. `fbtest bars
   320x200x8` shows the same bars doubled, centred, from the palette.
6. From the PC, `linux/tools/fbshot.sh root@192.168.68.59 bars.png`;
   compare it with the monitor.
7. `DOOM_FPS=1 doom` plays the demos; from an SSH terminal, the keys
   play. While it runs, `fbshot.sh` from a second SSH session catches a
   frame.
8. `doom -timedemo demo1` prints `timed 5026 gametics in N realtics (F
   fps)`. Zephyr's VGA build reached 24.7 fps in gameplay.
9. After Doom exits, `fbtest info` is back at 640x480, 16 bpp.

Goal: mainline Linux (`ARCH=klausscpu`) on the stage 3 release bitstream,
klaussCPU `mmu` 52bece8 LEAN:
- `CPU_FEATURES` = 0x0010_100D: exceptions and user mode, a 16-entry TLB
  with 64 KiB pages, LR/SC and the TRNG;
- no MPU, and no crypto blocks other than the TRNG.

It boots on the klausscc emulator first, then on the board, from an
initramfs, to a busybox shell on the UART.

## Facts the port is built on

| | |
|---|---|
| Core | 64-bit registers r0–r15, 32-bit addresses (EA[31:0]), little-endian, in order, 100 MHz. LP64: 64-bit pointers and `long`. |
| ABI (klausscpu-llvm) | Arguments in R0–R3, then the stack. Result in R12 (R11:R12 for 128-bit). Callee-saved R4–R7 and R15 (P, the frame pointer). SP is a separate register (`getsp`/`setsp`). |
| Modes | UM (user) and supervisor. `SP_ALT` is the inactive SP; it's SR `USP` in supervisor mode (MMU_PLAN §3.1). |
| Entry | Interrupts and exceptions alike (§3.3): from user mode, swap to the kernel SP, then push one 64-bit frame. The frame is PC [31:0], flags [38:32], INT_MASK [42:39], DI [43] and PM [44] (§3.2). An exception writes ECAUSE/EADDR, **sets DI** and jumps to `EVEC`. An interrupt clears its own `INT_MASK[n]`, leaves DI alone and jumps to `INT_VECn`. IRET restores everything, including `UM = PM` and the SP swap (§3.4). |
| SRs | STATUS, EVEC, ECAUSE, EADDR, USP, SCRATCH0/1 (the kernel's), TP (user TLS, context-switched), CYCLE (64-bit, user-readable), TLB_HI/LO/INDEX, ASID, TLBVEC (§6.1, §11.3). |
| TLB | 16 entries, fully associative, software refill. 64 KiB pages. 8-bit ASID plus a G bit. `TLBWI`/`TLBR`/`TLBP`/`TLBINV`. TLB_HI is auto-loaded on a miss, and misses go to `TLBVEC` if it's set (§11.3). |
| Address map (TE = 1, §11.2) | useg `0x0000_0000–0x7FFF_FFFF` (TLB). kseg `0x8000_0000–0x87FF_FFFF` (direct, PA = VA − 0x8000_0000, supervisor). `0x8800_0000–0xBFFF_FFFF` faults. kmap `0xC000_0000–0xEFFF_FFFF` (TLB, supervisor). MMIO `0xF000_0000+` (identity, supervisor). |
| Rules (§11.3) | Exception vectors, the refill handler, page tables and kernel stacks must be in kseg, because frame pushes aren't translated. A push outside kseg with TE = 1 is a double fault. TLB R/W/X are checked in supervisor mode too. |
| Memory | 128 MiB DDR at PA 0, all of it in kseg. D-cache 64 KiB 2-way, I-cache 8 KiB direct-mapped. The I-cache snoops CPU stores by line, but code that arrives by DMA, or is written through one mapping and run through another (kseg vs useg, as `test_tlb` T5 does), needs `ICACHE_INV`. |
| Devices | UART (polled, no RX interrupt), interval timer and 64-bit ms clock, interrupt controller (4 sources: timer, blitter, Ethernet, VGA; per-source vectors), LiteEth, SD card, QSPI flash, TRNG. |

## Workspace

- `linux-ws/` (gitignored) holds the upstream checkout, `linux-ws/linux`, a
  shallow clone of stable **v7.3.3**, and the build tree `linux-ws/build`
  (`O=`). Both are recreated by `linux/fetch.sh`.
- `linux/` (tracked) holds the port:
  - `linux/arch/klausscpu/` is the architecture. `fetch.sh` symlinks it into
    the checkout.
  - `linux/patches/NNNN-*.patch` are changes to generic code, applied in order.
  - `linux/tools/prebuilt-parser.py` stands in for bison/flex, which aren't
    installed. It emits kconfig's parser and lexer from the generated files
    in `/usr/src/linux-headers-*/scripts/kconfig`, with 7.3.3's action-code
    differences applied (the grammar is the same).
  - `linux/build.sh` is the `make` wrapper (`ARCH=klausscpu LLVM=<bin>/`,
    `HOSTCC=gcc`, `O=`).
  - `linux/toolchain-tests/` is the compiler, assembler and linker survey.

## Design

Model: **OpenRISC** for overall shape. It's a small arch with a software-
refilled TLB and leans on asm-generic. **MIPS64** is the model for a kernel
in a 32-bit direct-mapped segment (CKSEG0), the refill handler and the
`pgd_current` approach. **LoongArch** shows a 64-bit software-TLB port with
direct-mapped windows. RISC-V shows the generic syscall table and
`GENERIC_ENTRY`.

### Memory layout

- `CONFIG_64BIT`, with 32-bit hardware addresses. Kernel pointers are
  zero-extended kseg/kmap addresses.
  - `PAGE_OFFSET` = `0x8000_0000` and `PHYS_OFFSET` = 0, so `__pa(x)` = x − 0x8000_0000.
  - All 128 MiB is lowmem: no HIGHMEM, `FLATMEM`, no `SPARSEMEM_VMEMMAP`.
- `TASK_SIZE` = `0x8000_0000` (useg). `access_ok()` rejects anything at or
  above it, including pointers with bits [63:32] set, which the hardware
  would otherwise truncate.
- kmap `0xC000_0000–0xEFFF_FFFF` holds vmalloc, modules and fixmap/kmap
  (TLB-mapped, global entries).
- MMIO is identity-mapped at `0xF000_0000+` with no TLB entry needed, so
  `ioremap()` of an MMIO range can return the address itself.
- `THREAD_SIZE` = 16 KiB from the slab, in kseg. No `VMAP_STACK`, since
  kernel stacks must be in kseg.

### Pages and page tables

- `PAGE_SIZE` = 64 KiB (`HAVE_PAGE_SIZE_64KB`), the only TLB page size.
- PTEs are 8 bytes, 8192 per page, so one PTE page maps 512 MiB.
- **Two levels**: pgd → pte (`asm-generic/pgtable-nopmd.h`).
  `PGDIR_SHIFT` = 29, so the pgd has 8 entries covering 4 GiB.
  - Four entries are user space and two are kmap.
  - The first version allocates one pgd page; a small slab comes later.
  - `pgd_alloc` copies the kernel (kmap) entries from `swapper_pg_dir`.
- PTE low 32 bits = `TLB_LO` format: PFN [26:16], X [3], W [2], R [1], V [0].
  `MTSR tlb_lo` keeps only those bits, so the refill handler writes the PTE
  unmasked. Software bits are in [63:32]: PRESENT, DIRTY, YOUNG, SPECIAL,
  PROTNONE, and the swap encoding.
- Hardware W is set only when the PTE is both writable and DIRTY (the MIPS
  scheme). The first store faults, `handle_mm_fault` marks the page dirty,
  and `update_mmu_cache` rewrites the TLB entry.
- YOUNG: at first every present PTE counts as young; MIPS-style V-bit
  ageing can come later.

### TLB refill and faults

- **`TLBVEC` refill handler** (kseg, assembly):
  - A miss is always taken on a kseg kernel stack: from user mode the entry
    has already switched to it, and in supervisor mode SP is a kernel stack.
    So the handler may push.
  - It picks the pgd by VA: `swapper_pg_dir` for kmap (≥ 0xC000_0000),
    otherwise `pgd_current`, which `switch_mm` keeps in a variable.
  - pte = pgd[VA >> 29] → [(VA >> 16) & 8191]. If PRESENT: `MTSR tlb_lo`,
    then `TLBWI` at a round-robin index over the non-wired entries, then
    `IRET`.
  - Otherwise it branches to the general exception path, which becomes
    `do_page_fault`.
  - TLB_HI already holds {VPN, ASID}; G is set for kmap.
- **ASIDs** (8-bit) use a MIPS/arm64-style generation allocator, with a
  `TLBINV` on rollover. The first version just flushes on every
  `switch_mm`. `flush_tlb_page` uses `TLBP` plus an invalidating `TLBWI`.
- **Faults.** TLB_MISS_* with no PTE, and ACCESS_* (a permission or
  W-not-dirty fault), go to `do_page_fault` → `handle_mm_fault`. A kernel
  fault on a user address goes through the exception table (uaccess fixups).

### Exceptions, interrupts and system calls

- **One kseg entry stub** for EVEC and every INT_VEC. It saves r0–r15 and
  USP, the hardware frame, ECAUSE/EADDR, and `orig_r0`/`syscallno` in a
  `struct pt_regs` on the kernel stack. `GENERIC_ENTRY` handles the rest:
  `irqentry_enter/exit`, `syscall_enter_from_user_mode`, signals on return.
- **Interrupt entry.** The stub does `DI` first, because hardware entry
  doesn't. It records which source was taken, so each `INT_VECn` has its
  own short prologue. The handler unmasks the source when done, since
  entry auto-masked it.
- **System calls** use the asm-generic syscall table (`unistd.h`, as
  RISC-V/LoongArch/OpenRISC do).
  - **ABI:** `SYSCALL` with the number in **r12**, arguments in **r0–r5**,
    result in **r12**.
  - The frame PC already points past `SYSCALL`, so restarting rewinds by 4.
  - `orig_r0` and `syscallno` are kept for restart and ptrace.
- **`current`** lives in `SCRATCH0` (`mfsr`, one instruction), so no
  `-ffixed` register is needed (klausscpu-llvm doesn't support that).
  `SCRATCH1` is free. With no MPU, the entry stub doesn't need scratch
  registers for a guard-safe path.
- **Context switch.** `__switch_to` saves and restores R4–R7, R15, SP and
  the return address in `thread_struct`, plus `TP` (user TLS) and
  `SCRATCH0` (`current`). USP lives in each thread's `pt_regs`.
- **Signals.** `rt_sigframe` on the user stack, with `rt_sigreturn` through
  a trampoline in a vDSO page; at first in a fixed "sigpage" mapped into
  every process (no executable stack).
- **uaccess.** The kernel touches user memory directly through useg (TLB
  R/W/X apply to supervisor too), with an `__ex_table` fixup on each
  accessing instruction. Faults inside them are restartable and precise
  (§4.1).

### Time, interrupts, console

- **clocksource and `sched_clock`:** `CYCLE` (100 MHz, 64-bit,
  user-readable, so a vDSO `clock_gettime` is possible later).
- **clockevent:** the interval timer (`TIMER_PER`, periodic, source 0). Its
  frequency comes from Kconfig or a built-in constant.
- **irqchip:** a linear domain with 4 hwirqs over `INT_MASK`/`INT_PEND`
  (`GENERIC_IRQ_MULTI_HANDLER`).
- **Console:** earlycon/bootconsole on the polled UART, then a tty driver
  that polls RX from an hrtimer, since there's no RX interrupt. This works
  with `--uart-in` scripted input on the emulator.
- **No device tree at first:** memory is `memblock_add(0, 128 MiB)` and
  devices are platform devices registered in arch code. DT (built-in DTB)
  can come later if drivers need it; LiteEth in mainline uses DT.

### Atomics, SMP, caches

- `!SMP`. At first, atomics are the asm-generic UP versions, which disable
  interrupts. Then `atomic*`/`cmpxchg` move to LR/SC
  (`lr64`/`sc64`, `lr32`/`sc32`).
- Futexes use LR/SC on user addresses, with exception-table fixups.
- **Caches:**
  - DMA is coherent.
  - `flush_icache_range` (module load, `ptrace` pokes, signal trampolines,
    `exec` through the page cache) does an `ICACHE_INV`.
  - The D-cache is PIPT, so 64 KiB pages leave no alias problems (§11.1).

### Boot

- **Loading.** klausscc loads the `vmlinux` ELF by LMA. The kernel is
  linked at kseg VMAs (`0x8010_0000`), with `AT()` LMA = VMA − 0x8000_0000.
  The initramfs is built in (`INITRAMFS_SOURCE`). The board load path will
  need checking for a multi-MiB image: UART upload speed, or the netboot
  loader.
- **`head.S`** starts physical, with TE = 0:
  1. Wire one global identity TLB entry for its own 64 KiB page (§11.2:
     the next fetch after TE goes on is translated).
  2. Point `EVEC`/`TLBVEC` at kseg.
  3. Set TE, then jump to the kseg alias.
  4. Drop the wired entry.
  5. Clear BSS, set the init stack and `SCRATCH0` = `&init_task`, then
     call `start_kernel`.
- Probe `CPU_FEATURES` and refuse to boot without bits 0, 2 and 3.
- The command line is built in (`CMDLINE`).

### User space

- A **musl** port (`arch/klausscpu`):
  - `syscall_arch.h` uses register variables r0–r5 and r12, plus
    `syscall`.
  - `atomic_arch.h` uses LR/SC; `__set_thread_area` writes TP.
  - Also `crt_arch.h`, `setjmp`, `clone`, `syscall_cp`, `sigsetjmp`, the
    signal context, `reloc.h`, and the `bits/` headers.
  - Static linking first.
- **Compiler.** The `klausscpu-*-linux-*` triple compiles but doesn't
  define `__linux__`/`__unix__`, and the driver has no Linux toolchain
  (sysroot, crt files, `-static` link). That's a klausscpu-llvm change
  needed before musl and busybox.
- **busybox**, static, with a minimal config: sh, coreutils, mount, init.

## Toolchain status

See `linux/toolchain-tests/RESULTS.md` (`run.sh` regenerates it). As of
2026-11-09 the kernel-relevant surface mostly passes:
- passes: asm goto with and without outputs; `%c`/`%n`; `"i"(&sym)`;
  `X`/`m`; naked; the attributes the kernel uses; the builtins; jump
  tables; computed goto; macros and `.altmacro`; push/popsection; kernel
  linker-script features; `ld -r`; thin archives; and the binutils
  equivalents.
- Gaps:
  1. `.word`/`.hword` are unknown directives. Generic and ported `.S`
     code use them; `.long`/`.short` work.
  2. `-fpatchable-function-entry` is unsupported, so no ftrace for now.
  3. `-ffixed-<reg>` is unsupported; `current` lives in SCRATCH0 instead.
  4. `__thread` (TLS) is unsupported. The kernel doesn't need it; user
     space (musl) does, with TP-relative access.
  5. The Linux triple doesn't define `__linux__` and has no Linux driver
     toolchain (user space only).

## Findings while porting

- **ECAUSE numbering:** ILLEGAL 1, PRIVILEGED 2, TRAP 3, SYSCALL 4,
  MISALIGNED_LOAD/STORE 5/6, ACCESS_LOAD/STORE/FETCH 7/8/9,
  MISALIGNED_FETCH 10, TLB_MISS 11–13. `asm/klausscpu.h` uses these.
- **`bextr` only reaches bits below 32.** The compiler never uses it for
  higher bits, so the entry code tests frame[44] PM with `shrv`/`andi`.
- **`setr` sign-extends a 32-bit immediate; `setru` zero-extends.**
  Kernel addresses (0x8000_0000+) must use `setru`, as the compiler does
  for symbols.
- **klausscc loads by VMA, relative to the first segment**, at PA 0x20,
  and ignores `p_paddr`. So the kernel links at kseg `0x8000_0020`.
- **User binaries are ELF32**; `ELF_CLASS` = ELFCLASS32 with an LP64 ABI.
  `EM_KLAUSSCPU` = 0x4B43.
- **The fused branch conditions** (`fbr`/`fbri`) are EQ 0, LT 1, LE 2,
  ULT 3, ULE 4, and +8 inverts them (NE 8, GE 9, GT 10, UGE 11, UGT 12).
- **ELF32 files with an LP64 ABI:** the user stack's argc/argv/envp/auxv
  must be longs. Patch 0002 lets the arch set `elf_addr_t` (ARCH_ELF_ADDR_T
  = `unsigned long`); with 32-bit words, musl read argv[0] as NULL.
- **EI takes effect at WB, about 3 cycles later, on the stage 3 bitstream**
  (98fe470). An `ei; di` window never takes a pending interrupt, so the
  idle loop is `ei; wait`.
- **`fbr`/`fbri` can't branch to a global symbol** (no relocation for
  them), so asm uses a local label.
- **busybox's standalone shell runs applets through `/proc/self/exe`**, so
  rcS mounts `/proc` with `/bin/busybox mount` first.
- **`__clone` in musl is variadic**, so with this ABI its stack arguments
  start after the 32-byte home area (+40).
- **`WARN()` in `__exit` code puts `__bug_table` entries in
  `.exit.text`**, so the linker script keeps `.exit.text` and `.exit.data`.
- **klausscpu-llvm's clang 23 snapshot can't build with
  WARN_CONTEXT_ANALYSIS**: it rejects capability attributes on
  function-pointer parameters (blkdev.h). The option is off.
- **`INT_MASK` stays 0xF**, and a source is enabled by setting its
  `INT_VECn` (as in the Zephyr port). That way IRET never restores a
  stale mask from an old frame.

## Milestones

0. **Groundwork.** *(done)*
   - Workspace, patches and kconfig without bison/flex.
   - `make ARCH=klausscpu defconfig` succeeds.
   - Arch skeleton; record how far the build gets.
1. **Banner.** *(done)*
   - `head.S`, `setup_arch` (memblock, `paging_init`) and earlycon on the
     emulator.
   - Checkpoint: `Linux version 7.3.3 ... klausscpu` printed by the
     klausscc emulator.
2. **`start_kernel` completes.** *(done)*
   - Traps, IRQs, timer, scheduler, idle, slab, VFS and the initramfs
     unpacked.
   - Checkpoint: `Kernel panic - not syncing: No working init found` (or
     `Run /init`), with timer ticks advancing jiffies.
3. **First user process.** *(done on the emulator)*
   - User mode, SYSCALL, TLB refill, page faults, ELF exec.
   - A static libc-less `/init` in the initramfs (`write`, `getpid`,
     `fork`, `wait4`, `exit`) prints and loops.
   - Checkpoint: its output, and `fork` working.
4. **busybox.** *(done: musl, busybox init and ash, Dropbear)*
   - musl and the clang Linux toolchain.
   - Checkpoint: a busybox shell on the emulator, scripted with `--uart-in`.
5. **Board.** *(busybox, networking, SD card on d9cf6c2 done; QSPI host key and SD root file system need board checks; SD speed needs measuring)*
   - Same image on 52bece8; tune load speed and timer frequency.
   - Checkpoint: an interactive shell over the UART.
6. **Later.**
   - ASIDs and LR/SC atomics if milestones 2–4 didn't need them.
   - vDSO, LiteEth and networking, SD card, ftrace (once the compiler has
     patchable entries), modules.

## Risks

- **Emulator ≠ board** (no cache or cycle model). xv6 milestone 3 finds
  the first of these.
- **Kernel size versus load time.** `vmlinux` plus initramfs will be
  several MiB; UART upload may need the netboot path.
- **16 TLB entries with 64 KiB pages:** 1 MiB reach. The kernel uses kseg
  (no TLB) for nearly everything, so only user space and vmalloc compete.
  Watch the miss rate.
- **Compiler maturity.** The kernel is far larger than anything compiled so
  far. Expect codegen bugs; each gets a reduced test and a klausscpu-llvm
  fix, coordinated with the session that owns it.

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
    cascade; not run yet (no emulator model or bitstream so far);
  - the QSPI flash as read-only MTD (`qspi_flash.c`): its opcode check
    allows READ and RDID only, and partitions aren't writable;
  - ext4 and loop devices, for the root file system on the SD card.
- **User space:**
  - musl 1.2.5 (`linux/musl/`), threads included;
  - busybox (init with `/etc/init.d/rcS` and mdev; ash);
  - Dropbear 2026.94 (`linux/userland/dropbear.sh`): an SSH login over
    loopback passes on the emulator (`linux.sh <out> ssh`). It presents
    Zephyr's `ssh_shell` host key, read from the QSPI keystore (`kskey`,
    `linux.sh <out> keystore`);
  - `/init` is `sdinit` (`linux/userland/sdinit.c`): it boots an ext4
    image on the SD card, `/linux/rootfs.img` on the FAT partition, and
    falls back to the initramfs (`linux.sh <out> rootfs`).
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

### Board checks: root file system on the SD card

The card keeps its single FAT partition and its files. Linux only adds
the `/linux/` folder:
- `/linux/rootfs.img`, an ext4 image;
- `/linux/rootfs.img.new` while an install is in progress;
- `/linux/dropbear_ed25519_host_key`, only when the QSPI key isn't found.

The boot path:
- `sdinit`, the initramfs's `/init`, mounts `mmcblk0p1` (vfat) and
  attaches `/linux/rootfs.img` to a loop device. It mounts the image as
  ext4 and switches root to it, after which the card is at `/mnt`.
- Any failure falls back to the initramfs's busybox. The failures are no
  card, no image, a mount error, or no `/sbin/init`.
- Its messages start with `sdinit:`.

Build the image on the PC (busybox, and Dropbear if built; `/etc` comes
from `linux/initramfs/`):

```
linux/rootfs/build-rootfs.sh linux-ws/rootfs.img 96
sha256sum linux-ws/rootfs.img
cd linux-ws && python3 -m http.server 8000
```

On the board, booted from the initramfs with the network up:

```
install-rootfs http://<PC>:8000/rootfs.img <sha256>
```

If the PC's firewall blocks the board's connection (it does on Graham's
PC: wget times out), push the image from the PC over SSH instead,
compressed. Raw, 96 MiB at about 120 KiB/s takes about 14 minutes; the
gzipped image is a few MB:

```
gzip -1c linux-ws/rootfs.img | ssh root@192.168.68.59 'gunzip | install-rootfs - <sha256>'
```

It downloads to `rootfs.img.new` and checks the SHA-256. Only then does it
rename the file to `rootfs.img` and `sync`. Then reload the kernel
(`klausscc -i linux-ws/build/vmlinux -s -m`).

The checks:
- The console shows `EXT4-fs (loop0): mounted filesystem` and
  `sdinit: switching to the SD card's root file system`.
- `cat /etc/klausscpu-rootfs` prints the build date and runtime commit.
- `grep ' / ' /proc/mounts` shows ext4 on `/dev/loop0`.
- `ls /mnt/linux` lists the card's folder.
- Persistence: `echo x > /root/t; sync`, reload, then `cat /root/t`.
- Run `sync` before resetting the board, because the image is a file on
  vfat.

To update, run `install-rootfs` again from the running system. The old
image stays open until the next boot. To go back to the initramfs, rename
`/linux/rootfs.img` on the card, for example from a PC.

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
/sys/module/klausscpu_sd/parameters/{late_reads,multi_fallbacks,retries}
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
- **Unsupported block sizes.** Block sizes that aren't multiples of 8 are
  refused with -EINVAL. The core's only such request is ACMD22's 4 bytes,
  sent after a write error to count the blocks written. Refusing it makes
  the core redo the whole write. (Question for the CPU session: could the
  data engine take a 4-byte block?)
- **Card changes.** The driver doesn't poll for card changes: insert the
  card before boot.
- **Boot wait.** The mmc core finds the card asynchronously. When an mmc
  host exists, `sdinit` waits up to 3 s for `/dev/mmcblk0p1`.

The SPI driver now powers the slot off for 20 ms, in SPI mode
(`SD_CTRL[18]` = 0), before its first CMD0. This follows plan §4.1: a
load after native mode resets the controller, but only that power cycle
reliably resets a card left in any state.

Status: it builds (W=1 clean) and is probe-safe on the emulator (the
`busybox`, `rootfs` and `ssh` modes pass with the cascade off). It hasn't
run yet:
- the emulator's native SD model is coming;
- after that, the board on a `mmu-sd` bitstream.

An independent review of the driver against the plan found:
- ACMD22's 4-byte block (fixed as above);
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

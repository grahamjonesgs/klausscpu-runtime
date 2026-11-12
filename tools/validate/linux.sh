#!/usr/bin/env bash
# linux.sh <outdir> [shell|m3] [emu|board]
#
# Build the KlaussCPU Linux port (LINUX_PLAN.md: linux/fetch.sh's v7.3.3
# checkout, musl from linux/musl/build.sh) into <outdir>/build and run it on
# the klausscc emulator (emu, the default) or the board (board: loaded over
# the UART and typed with -m, under flock $BOARD_LOCK; needs a bitstream
# with the stage 3 TLB):
#   shell (default)  initramfs "minish": a scripted session at the "$ "
#                    prompt (klausscc --uart-in/--uart-prompt): built-ins,
#                    /proc, tmpfs, fork+execve of the musl test program, and
#                    unaligntest (misaligned access emulation)
#   busybox          initramfs "busybox": busybox init and ash (musl,
#                    linux/userland/busybox.sh), a scripted session
#   ssh              initramfs "busybox" with Dropbear (linux/userland/
#                    dropbear.sh): key, server and an SSH login over loopback
#   net              (board only) the busybox profile with Dropbear: boots,
#                    waits for rcS to bring eth0 up (DHCP) and start httpd
#                    and dropbear, then from this PC pings the board, fetches
#                    the httpd page and logs in over SSH (root/klausscpu)
#                    while the board's shell sleeps.  BOARD_IP (default
#                    192.168.68.59, the DHCP reservation for its MAC).
#   keystore         Zephyr's QSPI keystore format (a synthetic NVS image)
#                    through kskey and dropbearconvert: Dropbear's fingerprint
#                    must equal OpenSSH's for the same key
#   rootfs           sdinit switches to an ext4 rootfs image (linux/rootfs/
#                    build-rootfs.sh) loop-mounted from the initramfs
#                    (the SD card's /linux/rootfs.img on the board)
#   sd-spi           (emulator) an SD card image (klausscc --sd-image: MBR,
#                    one FAT32 partition) through the SPI driver, in two
#                    boots: (1) from the initramfs, install-rootfs writes
#                    the rootfs image to /linux/ on the card, which is read
#                    back after dropping caches (also with the multiblock
#                    and pipeline switches off), then the raw device is
#                    hashed and compared with the image file; (2) sdinit
#                    switches to the card's /linux/rootfs.img.  fsck.fat
#                    checks the card between the boots
#   sd-native        the same through the native 4-bit controller
#                    (klausscc --sd-native) and the mmc host driver, plus
#                    sdcmd (MMC_IOC_CMD): ACMD51, ACMD13, and ACMD22's
#                    4-byte block after an 8-sector O_DIRECT write
#                    Both SD modes then boot once per injected fault
#                    (klausscc --sd-fault: a rejected written block, a
#                    write busy that never ends, a bad read block, a
#                    command timeout) on a fresh card, write and read back
#                    a 2 MiB file, and check its hash and fsck.fat
#   ports            the ported programs (linux/userland/<pkg>.sh: lua, ...),
#                    each built here if it isn't yet, in a system image that
#                    sdinit boots from the initramfs with a data image at
#                    /data; one boot per port runs its self-test from
#                    /usr/share/klausscpu/tests
#   buildroot        the Buildroot packages (linux/buildroot/build.sh) merged
#                    into the system image (ROOTFS_BUILDROOT=1), booted as
#                    in ports; tests/buildroot-test.sh checks each one
#   m3               the libc-free milestone-3 /init (no input)
# LINUX_BUSYBOX_PREBUILT=1 reuses linux-ws/busybox-build/busybox instead of
# running linux/userland/busybox.sh.
# Each expected line must appear in the UART output, in order
# (<outdir>/<step>.out has the run, <outdir>/<step>.expect the lines).
# Needs linux-ws/linux (linux/fetch.sh).  Exits 1 on any failure.
set -uo pipefail
EMU_ARGS=()
. "$(dirname "$0")/common.sh"
[ $# -ge 1 ] || die "usage: linux.sh <outdir> [shell|busybox|ssh|net|keystore|rootfs|sd-spi|sd-native|m3] [emu|board]"
OUT=$(realpath -m "$1"); MODE=${2:-shell}; WHERE=${3:-emu}
case $MODE in shell|busybox|ssh|net|keystore|rootfs|sd-spi|sd-native|ports|buildroot|m3) ;; *) die "unknown mode $MODE" ;; esac
[ "$MODE" != net ] || [ "$WHERE" = board ] || die "net needs the board (the emulator has no Ethernet)"
SD=0
case $MODE in sd-*) SD=1 ;; esac
[ $SD = 0 ] || [ "$WHERE" = emu ] || die "$MODE runs on the emulator (on the board the card holds real files)"
[ $SD = 0 ] || "$KLAUSSCC" --help 2>/dev/null | grep -q -- '--sd-native' ||
  die "$KLAUSSCC has no --sd-image/--sd-native: build klausscc's mmu branch"
case $WHERE in emu|board) ;; *) die "unknown target $WHERE" ;; esac
"$KLAUSSCC" --help 2>/dev/null | grep -q -- '--uart-prompt' ||
  die "$KLAUSSCC has no --uart-prompt: build klausscc's mmu branch and set KLAUSSCC"
[ -d "$RT/linux-ws/linux/.git" ] || die "no kernel checkout: run linux/fetch.sh"
rm -rf "$OUT"; mkdir -p "$OUT"
fail=0
bad() { echo "FAIL $*"; fail=$((fail + 1)); }

export KLAUSSCPU_LLVM_BIN=$LLVM_BIN
if [ "$MODE" = shell ] || [ "$MODE" = busybox ] || [ "$MODE" = ssh ] || [ "$MODE" = net ] || [ "$MODE" = keystore ] || [ "$MODE" = rootfs ] || [ $SD = 1 ] || [ "$MODE" = ports ] || [ "$MODE" = buildroot ]; then
  "$RT/linux/musl/build.sh" >"$OUT/musl.log" 2>&1 || { echo "FAIL musl build (see $OUT/musl.log)"; exit 1; }
  export INITRAMFS_PROFILE=minish
fi
if [ "$MODE" = busybox ] || [ "$MODE" = ssh ] || [ "$MODE" = net ] || [ "$MODE" = keystore ] || [ "$MODE" = rootfs ] || [ $SD = 1 ] || [ "$MODE" = ports ] || [ "$MODE" = buildroot ]; then
  if [ "${LINUX_BUSYBOX_PREBUILT:-0}" = 1 ] && [ -x "$RT/linux-ws/busybox-build/busybox" ]; then
    echo "busybox: reusing linux-ws/busybox-build/busybox (LINUX_BUSYBOX_PREBUILT=1)"
  else
    "$RT/linux/userland/busybox.sh" >"$OUT/busybox.log" 2>&1 || { echo "FAIL busybox build (see $OUT/busybox.log)"; exit 1; }
  fi
  if [ "$MODE" = ssh ] || [ "$MODE" = net ] || [ "$MODE" = keystore ]; then
    "$RT/linux/userland/dropbear.sh" >"$OUT/dropbear.log" 2>&1 || { echo "FAIL dropbear build (see $OUT/dropbear.log)"; exit 1; }
  fi
  if [ "$MODE" = keystore ]; then
    # A test key in a synthetic Zephyr NVS keystore image, and its
    # fingerprint as OpenSSH computes it.
    openssl ecparam -name prime256v1 -genkey -noout -outform DER -out "$OUT/key.der" 2>/dev/null &&
      openssl ec -inform DER -in "$OUT/key.der" -out "$OUT/key.pem" 2>/dev/null &&
      chmod 600 "$OUT/key.pem" &&
      python3 -I "$RT/linux/userland/keystore/mknvs.py" "$OUT/storage.img" newest "$OUT/key.der" ||
      { echo "FAIL keystore test image"; exit 1; }
    KEY_FP=$(ssh-keygen -y -f "$OUT/key.pem" | ssh-keygen -lf - | awk '{print $2}')
    printf 'dir /test 0755 0 0\nfile /test/storage.img %s 0644 0 0\n' "$OUT/storage.img" >"$OUT/extra.list"
    export INITRAMFS_EXTRA=$OUT/extra.list
  fi
  if [ "$MODE" = rootfs ]; then
    # A small rootfs image in the initramfs at /test/rootfs.img, which
    # sdinit tries before the SD card (the emulator has none).
    "$RT/linux/rootfs/build-rootfs.sh" "$OUT/rootfs.img" 16 >"$OUT/rootfs.log" 2>&1 ||
      { echo "FAIL rootfs image (see $OUT/rootfs.log)"; exit 1; }
    printf 'dir /test 0755 0 0\nfile /test/rootfs.img %s 0644 0 0\n' "$OUT/rootfs.img" >"$OUT/extra.list"
    export INITRAMFS_EXTRA=$OUT/extra.list
  fi
  if [ "$MODE" = ports ]; then
    # Each port: linux/userland/<pkg>.sh, run if its binary isn't built.
    for p in lua:lua-build/lua sqlite:sqlite-build/sqlite3 coremark:coremark-build/coremark \
             lmbench:lmbench-build/tlbmiss iperf3:iperf3-build/iperf3 curl:curl-build/curl; do
      if [ ! -x "$RT/linux-ws/${p#*:}" ]; then
        "$RT/linux/userland/${p%%:*}.sh" >"$OUT/${p%%:*}.log" 2>&1 ||
          { echo "FAIL ${p%%:*} build (see $OUT/${p%%:*}.log)"; exit 1; }
      fi
    done
    "$RT/linux/rootfs/build-rootfs.sh" "$OUT/rootfs.img" 24 >"$OUT/rootfs.log" 2>&1 &&
      "$RT/linux/rootfs/build-data.sh" "$OUT/data.img" 8 >>"$OUT/rootfs.log" 2>&1 ||
      { echo "FAIL rootfs/data images (see $OUT/rootfs.log)"; exit 1; }
    printf 'dir /test 0755 0 0\nfile /test/rootfs.img %s 0644 0 0\nfile /test/data.img %s 0644 0 0\n' \
      "$OUT/rootfs.img" "$OUT/data.img" >"$OUT/extra.list"
    export INITRAMFS_EXTRA=$OUT/extra.list
  fi
  if [ "$MODE" = buildroot ]; then
    # Buildroot's packages (built unless LINUX_BUILDROOT_PREBUILT=1 and
    # there is an image) merged into a system image, booted as in ports.
    # With the development tools (2026-11-11) the image holds ~160 MiB, more
    # than the emulated machine's 128 MiB of RAM, so it can't ride in the
    # initramfs; putting it on an emulated SD card needs a FAT writer
    # (mtools) on this PC.  Until then the check is the board's
    # /usr/share/klausscpu/tests/buildroot-test.sh.
    if [ "${LINUX_BUILDROOT_EMU_FORCE:-0}" != 1 ]; then
      echo "buildroot: the full system image no longer fits in the emulator's RAM;"
      echo "run tests/buildroot-test.sh on the board (LINUX_BUILDROOT_EMU_FORCE=1 tries anyway)"
      exit 2
    fi
    if [ "${BR_SHARED:-1}" = 1 ]; then BRO=$RT/linux-ws/br-output-dyn; else BRO=$RT/linux-ws/br-output; fi
    if [ "${LINUX_BUILDROOT_PREBUILT:-0}" != 1 ] || [ ! -f "$BRO/images/rootfs.tar" ]; then
      "$RT/linux/buildroot/build.sh" >"$OUT/buildroot.log" 2>&1 ||
        { echo "FAIL buildroot build (see $OUT/buildroot.log)"; exit 1; }
    fi
    ROOTFS_BUILDROOT=1 "$RT/linux/rootfs/build-rootfs.sh" "$OUT/rootfs.img" 32 >"$OUT/rootfs.log" 2>&1 &&
      "$RT/linux/rootfs/build-data.sh" "$OUT/data.img" 8 >>"$OUT/rootfs.log" 2>&1 ||
      { echo "FAIL rootfs/data images (see $OUT/rootfs.log)"; exit 1; }
    printf 'dir /test 0755 0 0\nfile /test/rootfs.img %s 0644 0 0\nfile /test/data.img %s 0644 0 0\n' \
      "$OUT/rootfs.img" "$OUT/data.img" >"$OUT/extra.list"
    export INITRAMFS_EXTRA=$OUT/extra.list
  fi
  if [ $SD = 1 ]; then
    # The images ride in the initramfs under /seed (not /test, which
    # sdinit would boot): system image A raw, a newer system image B and
    # a data image gzipped; the card starts with an empty FAT32 partition
    # at sector 2048, as on the board's card.
    KLAUSSCPU_ROOTFS_TAG=A "$RT/linux/rootfs/build-rootfs.sh" "$OUT/rootfs.img" 16 >"$OUT/rootfs.log" 2>&1 &&
      KLAUSSCPU_ROOTFS_TAG=B "$RT/linux/rootfs/build-rootfs.sh" "$OUT/rootfs-b.img" 16 >>"$OUT/rootfs.log" 2>&1 &&
      "$RT/linux/rootfs/build-data.sh" "$OUT/data.img" 16 >>"$OUT/rootfs.log" 2>&1 &&
      gzip -1f "$OUT/rootfs-b.img" "$OUT/data.img" ||
      { echo "FAIL rootfs/data images (see $OUT/rootfs.log)"; exit 1; }
    {
      echo 'dir /seed 0755 0 0'
      echo "file /seed/rootfs.img $OUT/rootfs.img 0644 0 0"
      echo "file /seed/rootfs-b.img.gz $OUT/rootfs-b.img.gz 0644 0 0"
      echo "file /seed/data.img.gz $OUT/data.img.gz 0644 0 0"
    } >"$OUT/extra.list"
    if [ "$MODE" = sd-native ]; then
      # sdcmd: ACMD51/13/22 (8, 64 and 4-byte blocks) through MMC_IOC_CMD.
      # Test-only, since its wtest writes raw sectors.
      "$LLVM_BIN/clang" --target=klausscpu-unknown-linux-musl --sysroot="$RT/linux-ws/sysroot" -Os \
        "$RT/linux/userland/sdcmd.c" -o "$OUT/sdcmd" >"$OUT/sdcmd.log" 2>&1 ||
        { echo "FAIL sdcmd build (see $OUT/sdcmd.log)"; exit 1; }
      printf 'file /seed/sdcmd %s 0755 0 0\n' "$OUT/sdcmd" >>"$OUT/extra.list"
    fi
    export INITRAMFS_EXTRA=$OUT/extra.list
    CARD=$OUT/card.img
    truncate -s 64M "$CARD" &&
      echo 'start=2048, type=c' | sfdisk -q "$CARD" >"$OUT/card.log" 2>&1 &&
      mkfs.vfat -F 32 -s 1 -n KLAUSSCPU --offset 2048 "$CARD" $(( 65536 - 1024 )) >>"$OUT/card.log" 2>&1 ||
      { echo "FAIL card image (see $OUT/card.log)"; exit 1; }
    cp "$CARD" "$OUT/card-empty.img"	# for the fault boots
    EMU_ARGS=(--sd-image "$CARD")
    [ "$MODE" = sd-native ] && EMU_ARGS+=(--sd-native)
  fi
  export INITRAMFS_PROFILE=busybox
elif [ "$MODE" = m3 ]; then
  export INITRAMFS_PROFILE=m3
fi
export LINUX_BUILD=$OUT/build
"$RT/linux/build.sh" defconfig >"$OUT/build.log" 2>&1 &&
  "$RT/linux/build.sh" vmlinux >>"$OUT/build.log" 2>&1 ||
  { echo "FAIL kernel build (see $OUT/build.log)"; exit 1; }
cp "$OUT/build/vmlinux" "$OUT/vmlinux"

# expect_in_order <expect> <out>: every line of <expect> is a whole line of
# <out> (CRs, trailing blanks, a "[ time]" printk prefix and the prompt
# prefix ignored: "$ " or profile's "user@host:dir $ "), in order.
expect_in_order() {
  awk 'BEGIN { n = 0; i = 0 }
       NR == FNR { sub(/[ \t]+$/, ""); want[n++] = $0; next }
       { sub(/\r$/, ""); sub(/[ \t]+$/, ""); line = $0
         sub(/^\[ *[0-9]+\.[0-9]+\] /, "", line)
         while (sub(/^[^ ]*@[^ ]*:[^ ]* \$ /, "", line) || sub(/^\$ /, "", line)) {}
         if (i < n && line == want[i]) i++ }
       END { if (i < n) { print "missing: " want[i]; exit 1 } }' "$1" "$2"
}

# run_step <name> <max instructions> <timeout s> [prompt]: <name>.in typed
# at the prompt (no input without a prompt).
run_step() {
  local input=()
  [ -n "${4:-}" ] && input=(--uart-in "$OUT/$1.in" --uart-prompt "$4")
  if [ "$WHERE" = board ]; then
    flock "$BOARD_LOCK" timeout "$(( $3 * 3 + 120 ))" "$KLAUSSCC" -i "$OUT/vmlinux" -s -m \
      "${input[@]}" </dev/null >"$OUT/$1.out" 2>&1
  else
    timeout "$3" "$KLAUSSCC" -i "$OUT/vmlinux" --emulate --max-instructions "$2" \
      "${EMU_ARGS[@]}" "${input[@]}" </dev/null >"$OUT/$1.out" 2>&1
    if [ -n "${4:-}" ]; then
      grep -aq 'stop = InputDone' "$OUT/$1.out" || bad "$1: the session did not finish ($(grep -ao 'stop = [A-Za-z]*' "$OUT/$1.out"))"
    fi
    echo "$1: $(grep -ao 'Emulator finished: [0-9]* instructions' "$OUT/$1.out")"
  fi
  local why
  why=$(expect_in_order "$OUT/$1.expect" "$OUT/$1.out") || bad "$1: $why"
  grep -aq 'Kernel panic\|Oops' "$OUT/$1.out" && bad "$1: kernel panic"
}

if [ "$MODE" = shell ]; then
  cat >"$OUT/shell.in" <<'EOF2'
echo hello from minish
uname
ls /bin
mount
cat /proc/self/comm
/bin/musltest
/bin/unaligntest -q
mkdir /tmp/d
ls /tmp
rm /tmp/d
ls /tmp
EOF2
  cat >"$OUT/shell.expect" <<'EOF2'
Run /init as init process
minish: KlaussCPU Linux shell (built-ins: echo cd pwd ls cat mkdir rm uname mount ps exit; or a /path/to/program)
echo hello from minish
hello from minish
uname
Linux (none) 7.3.3-dirty klausscpu
ls /bin
mount
cat /proc/self/comm
init
/bin/musltest
musltest: malloc+memset ok, strlen=99999
musltest: signal handler ran: yes
musltest: setjmp/longjmp ok
musltest: child exited with 5
musltest: tcp loopback echo ok
musltest: 4 threads: mutex count 8000, atomic count 8000
musltest: uaccess at a page edge ok
musltest: 300 forks, 0 bad
musltest: PASS
/bin/unaligntest -q
unaligntest: PASS
mkdir /tmp/d
ls /tmp
d
rm /tmp/d
ls /tmp
EOF2
  run_step shell 3000000000 600 '$ '
elif [ "$MODE" = busybox ]; then
  cat >"$OUT/busybox.in" <<'EOF2'
echo hello from busybox
uname -sm
hostname
ls /etc/init.d
echo 1 2 3 | wc -w
mkdir /tmp/t && echo hi > /tmp/t/f && cat /tmp/t/f
cat /proc/self/comm
sh -c 'exit 3'; echo status $?
EOF2
  cat >"$OUT/busybox.expect" <<'EOF2'
Run /init as init process
echo hello from busybox
hello from busybox
uname -sm
Linux klausscpu
hostname
klausscpu
ls /etc/init.d
hostkey  rcS
echo 1 2 3 | wc -w
3
mkdir /tmp/t && echo hi > /tmp/t/f && cat /tmp/t/f
hi
cat /proc/self/comm
cat
sh -c 'exit 3'; echo status $?
status 3
EOF2
  run_step busybox 20000000000 900 '$ '
elif [ "$MODE" = rootfs ]; then
  # sdinit loop-mounts the ext4 image and switches to it; busybox init and
  # rcS then run from the image.
  cat >"$OUT/rootfs.in" <<'EOF2'
cut -d' ' -f1 /etc/klausscpu-rootfs
grep ' / ' /proc/mounts | cut -d' ' -f3
/usr/bin/musltest | tail -1
EOF2
  cat >"$OUT/rootfs.expect" <<'EOF2'
sdinit: switching to the SD card's root file system (read-only)
klausscpu-rootfs
ext4
musltest: PASS
EOF2
  run_step rootfs 20000000000 900 '$ '
elif [ $SD = 1 ]; then
  P=/sys/module/klausscpu_sd/parameters
  SHA=$(sha256sum "$OUT/rootfs.img" | cut -c1-16)
  HEAD=$(head -c 262144 "$OUT/rootfs.img" | sha256sum | cut -c1-16)
  # Boot 1: the card has no image yet, so sdinit stays in the initramfs.
  {
    # The native driver's command errors after a clean boot (the mmc
    # core's probing), the baseline for the cmdto fault boots below.
    [ "$MODE" = sd-native ] && echo "echo base \$(cat $P/native_cmd_errors)"
    echo 'cat /sys/class/block/mmcblk0p1/start'
    echo 'install-rootfs - < /seed/rootfs.img'
    echo 'sync; echo 3 > /proc/sys/vm/drop_caches; sha256sum /mnt/linux/rootfs.img | cut -c1-16'
    if [ "$MODE" = sd-spi ]; then
      echo "echo 0 > $P/pipeline; echo 0 > $P/multiblock; echo 3 > /proc/sys/vm/drop_caches"
      echo 'sha256sum /mnt/linux/rootfs.img | cut -c1-16'
      echo 'head -c 262144 /seed/rootfs.img > /mnt/linux/b.bin; sync; echo 3 > /proc/sys/vm/drop_caches'
      echo "echo 1 > $P/pipeline; echo 1 > $P/multiblock"
      echo 'sha256sum /mnt/linux/b.bin | cut -c1-16'
      echo "cat $P/late_reads $P/multi_fallbacks $P/retries"
    else
      echo 'head -c 262144 /seed/rootfs.img > /mnt/linux/b.bin; sync; echo 3 > /proc/sys/vm/drop_caches'
      echo 'sha256sum /mnt/linux/b.bin | cut -c1-16'
      # The card's last 8 KiB: FAT free space, past the raw hash below.
      echo '/seed/sdcmd scr /dev/mmcblk0; /seed/sdcmd ssr /dev/mmcblk0'
      echo '/seed/sdcmd wtest /dev/mmcblk0 131056'
    fi
    # The data image, once; a second install must refuse.  System image B
    # goes on the card for boot 2 to install (the initramfs is gone then).
    echo 'install-data -z - < /seed/data.img.gz'
    echo 'install-data -z - < /seed/data.img.gz; echo status $?'
    echo 'cp /seed/rootfs-b.img.gz /mnt/linux/; sync'
    echo 'umount /mnt; head -c 8388608 /dev/mmcblk0 | sha256sum | cut -c1-16'
  } >"$OUT/sd1.in"
  {
    # Probe lines come before /init.
    if [ "$MODE" = sd-spi ]; then
      echo 'klausscpu-sd: mmcblk0, 64 MiB (SDHC/SDXC), multiblock 1, pipeline 1'
    else
      echo 'klausscpu-intc: source 2 cascades Ethernet and SD'
      echo 'klausscpu-sd: the native SD controller has the card'
      echo 'klausscpu-sdnative klausscpu-sdnative: native SD controller v1, 8-block FIFO, 4-bit, IRQ 3'
      echo 'mmc0: new SDHC card at address 1234'
    fi
    echo 'Run /init as init process'
    echo 'sdinit: no usable /linux/rootfs.img on the SD card'
    echo 'sdinit: staying in the initramfs'
    echo 'cat /sys/class/block/mmcblk0p1/start'
    echo '2048'
    echo 'install-rootfs: /linux/rootfs.img installed; reboot to use it'
    echo "$SHA"
    if [ "$MODE" = sd-spi ]; then
      echo "$SHA"
      echo "$HEAD"
      echo '0'
      echo '0'
      echo '0'
    else
      echo "$HEAD"
      echo 'acmd22: 8 blocks written'
      echo 'wtest: read back same'
    fi
    echo 'install-data: gzip CRC-32 and length OK'
    echo 'install-data: /linux/data.img installed; reboot to use it'
    echo 'install-data: /mnt/linux/data.img exists and holds user data: not replaced (--force replaces it)'
    echo 'status 1'
  } >"$OUT/sd1.expect"
  run_step sd1 100000000000 3000 '$ '
  # The raw read of the card's first 8 MiB, against the image file.
  RAW=$(head -c 8388608 "$CARD" | sha256sum | cut -c1-16)
  grep -aq "$RAW" "$OUT/sd1.out" || bad "sd1: the raw read of mmcblk0 differs from the card image ($RAW)"
  dd if="$CARD" of="$OUT/part.img" bs=512 skip=2048 status=none &&
    fsck.fat -n "$OUT/part.img" >"$OUT/fsck.log" 2>&1 || bad "sd1: fsck.fat on the card's partition (see $OUT/fsck.log)"
  rm -f "$OUT/part.img"
  if [ "$MODE" = sd-native ]; then
    grep -aq '^scr: ' "$OUT/sd1.out" || bad "sd1: sdcmd scr (ACMD51, 8 bytes)"
    grep -aq '^ssr: ' "$OUT/sd1.out" || bad "sd1: sdcmd ssr (ACMD13, 64 bytes)"
  fi

  # Boot 2: sdinit mounts system image A read-only and data.img at /data.
  cat >"$OUT/sd2.in" <<'EOF2'
cut -d' ' -f1,6 /etc/klausscpu-rootfs
grep ' / ' /proc/mounts | cut -d' ' -f1,3,4 | cut -d, -f1
grep ' /data ' /proc/mounts | cut -d' ' -f1,3,4 | cut -d, -f1
touch /usr/x 2>/dev/null && echo usr writable || echo usr read-only
echo persist > /root/t; sync; cat /root/t
ls /mnt/linux/rootfs.img
/usr/bin/musltest | tail -1
echo HOSTNAME=klaustest > /data/etc/network; echo 'echo rc.local ran > /tmp/rc.local.out' > /data/etc/rc.local; sync
install-rootfs -z - < /mnt/linux/rootfs-b.img.gz
EOF2
  cat >"$OUT/sd2.expect" <<'EOF2'
sdinit: data.img mounted read-write at /data
sdinit: switching to the SD card's root file system (read-only)
klausscpu-rootfs A
/dev/loop0 ext4 ro
/dev/loop1 ext4 rw
usr read-only
persist
/mnt/linux/rootfs.img
musltest: PASS
install-rootfs: staged as /linux/rootfs.img.next; it replaces rootfs.img at the next boot
EOF2
  run_step sd2 60000000000 2400 '$ '

  # Boot 3: sdinit swaps in system image B; /root and the settings in
  # /data/etc survive.  Then a broken /data/etc/network for boot 4.
  # rc.local runs in the background (rcS), so its output would race the
  # shell's: it writes a file, read here once it has had time to run.
  cat >"$OUT/sd3.in" <<'EOF2'
sleep 2; cat /tmp/rc.local.out
cut -d' ' -f1,6 /etc/klausscpu-rootfs
cat /root/t
hostname
touch /usr/x 2>/dev/null && echo usr writable || echo usr read-only
printf 'IP="192.168\n' > /data/etc/network; sync; echo broken
EOF2
  cat >"$OUT/sd3.expect" <<'EOF2'
sdinit: installed the staged /linux/rootfs.img.next
sdinit: data.img mounted read-write at /data
rcS: settings from /data/etc/network
rcS: running /data/etc/rc.local
rc.local ran
klausscpu-rootfs B
persist
klaustest
usr read-only
broken
EOF2
  run_step sd3 60000000000 2400 '$ '

  # Boot 4: the broken settings file gets a warning and the defaults; the
  # boot goes on.  Then data.img is renamed away for boot 5.
  cat >"$OUT/sd4.in" <<'EOF2'
hostname
cat /root/t
mv /mnt/linux/data.img /mnt/linux/data.img.off; sync; echo moved
EOF2
  cat >"$OUT/sd4.expect" <<'EOF2'
sdinit: data.img mounted read-write at /data
rcS: WARNING: /data/etc/network failed to load; using the defaults
rcS: running /data/etc/rc.local
klausscpu
persist
moved
EOF2
  run_step sd4 60000000000 2400 '$ '

  # Boot 5: no data.img: still boots, /data (so /root) in RAM, warned.
  cat >"$OUT/sd5.in" <<'EOF2'
cut -d' ' -f1,6 /etc/klausscpu-rootfs
grep ' /data ' /proc/mounts | cut -d' ' -f1,3
echo x > /root/y && cat /root/y
hostname
EOF2
  cat >"$OUT/sd5.expect" <<'EOF2'
sdinit: WARNING: no /mnt/sd/linux/data.img
sdinit: WARNING: /data is a tmpfs: /root, /home and /data/etc are lost at reboot
sdinit: switching to the SD card's root file system (read-only)
rcS: WARNING: /data is in RAM (no usable /linux/data.img on the SD card):
klausscpu-rootfs B
tmpfs tmpfs
x
klausscpu
EOF2
  run_step sd5 60000000000 2400 '$ '
  # The emulator stops with the card mounted (the system image lives on
  # it), as a board reset would: clear vfat's dirty flag (boot sector byte
  # 0x41) in the copy, so fsck.fat reports only real damage.
  dd if="$CARD" of="$OUT/part.img" bs=512 skip=2048 status=none
  printf '\0' | dd of="$OUT/part.img" bs=1 seek=65 conv=notrunc status=none
  fsck.fat -n "$OUT/part.img" >"$OUT/fsck5.log" 2>&1 || bad "sd5: fsck.fat after the boots (see $OUT/fsck5.log)"
  rm -f "$OUT/part.img"

  # Fault boots (klausscc --sd-fault; transient: a retry succeeds).  Each
  # starts from the empty card, so sdinit stays in the initramfs and boot
  # touches few blocks; the session writes a 2 MiB file (4096 blocks) and
  # reads it back after dropping caches, so the event numbers below land
  # in that file's transfers.  Linux must recover: the data reads back
  # right, and the FAT stays clean.
  if "$KLAUSSCC" --help 2>/dev/null | grep -q -- '--sd-fault'; then
    H2=$(head -c 2097152 "$OUT/rootfs.img" | sha256sum | cut -c1-16)
    # wbusy@1 hits a single-block write: sdinit's vfat mount marking the
    # file system dirty.  The rest land in the 2 MiB file's transfers.
    FAULTS="wcrc@1000 wbusy@1000 wbusy@1 rerr@3000 cmdto:18@8"
    [ "$MODE" = sd-native ] && FAULTS="$FAULTS cmdto:13@5"
    BASE=$(grep -ao '^base [0-9]*' "$OUT/sd1.out" | awk '{print $2}')
    {
      echo 'cat /sys/block/mmcblk0/stat'
      echo 'mount -t vfat /dev/mmcblk0p1 /mnt; head -c 2097152 /seed/rootfs.img > /mnt/t.bin; sync; echo wrote'
      echo 'cat /sys/block/mmcblk0/stat'
      echo 'echo 3 > /proc/sys/vm/drop_caches; sha256sum /mnt/t.bin | cut -c1-16'
      echo 'cat /sys/block/mmcblk0/stat'
      echo 'umount /mnt; echo unmounted'
      # Evidence that the fault went through the error path: the mmc core
      # recovers silently, and so do the SPI driver's retries.
      if [ "$MODE" = sd-spi ]; then
        echo "echo stats \$(cat $P/multi_retries) \$(cat $P/retries) \$(cat $P/multi_fallbacks)"
      else
        echo "echo stats \$(cat $P/native_data_errors) \$(cat $P/native_cmd_errors) \$(cat $P/native_timeouts)"
      fi
    } >"$OUT/fault.in"
    for f in $FAULTS; do
      n=fault-$(echo "$f" | tr ':@' '--')
      cp "$OUT/card-empty.img" "$OUT/$n.img"
      EMU_ARGS=(--sd-image "$OUT/$n.img" --sd-fault "$f")
      [ "$MODE" = sd-native ] && EMU_ARGS+=(--sd-native)
      cp "$OUT/fault.in" "$OUT/$n.in"
      printf 'wrote\n%s\nunmounted\n' "$H2" >"$OUT/$n.expect"
      run_step "$n" 40000000000 2400 '$ '
      grep -aq 'klausscc sd: injected' "$OUT/$n.out" || bad "$n: the fault was never injected"
      read -r a b c < <(grep -ao '^stats [0-9]* [0-9]* [0-9]*' "$OUT/$n.out" | awk '{print $2, $3, $4}')
      echo "$n: stats ${a:-?} ${b:-?} ${c:-?} ($([ "$MODE" = sd-spi ] && echo 'multi_retries retries multi_fallbacks' || echo "data_errors cmd_errors (base ${BASE:-?}) timeouts"))"
      if [ "$MODE" = sd-spi ]; then
        [ "$(( ${a:-0} + ${b:-0} ))" -ge 1 ] || bad "$n: no retry counted"
      else
        case $f in
        cmdto*) [ "${b:-0}" -gt "${BASE:-0}" ] || bad "$n: no command error counted" ;;
        *) [ "${a:-0}" -ge 1 ] || bad "$n: no data error counted" ;;
        esac
        [ "${c:-1}" = 0 ] || bad "$n: the watchdog fired"
      fi
      dd if="$OUT/$n.img" of="$OUT/part.img" bs=512 skip=2048 status=none &&
        fsck.fat -n "$OUT/part.img" >"$OUT/$n.fsck" 2>&1 || bad "$n: fsck.fat (see $OUT/$n.fsck)"
      rm -f "$OUT/part.img" "$OUT/$n.img"
    done
  else
    echo "sd faults: skipped ($KLAUSSCC has no --sd-fault)"
  fi
elif [ "$MODE" = buildroot ]; then
  # Every installed package passes (LINUX_BUILDROOT_EXPECT=N: at least N).
  echo /usr/share/klausscpu/tests/buildroot-test.sh >"$OUT/br.in"
  echo 'sdinit: data.img mounted read-write at /data' >"$OUT/br.expect"
  run_step br 120000000000 3600 '$ '
  line=$(grep -ao 'buildroot-test: [0-9]*/[0-9]* passed' "$OUT/br.out" | tail -1)
  p=${line#buildroot-test: }; p=${p%%/*}; m=${line#*/}; m=${m%% *}
  echo "br: ${line:-no summary}"
  [ -n "$line" ] && [ "$p" = "$m" ] && [ "$p" -ge "${LINUX_BUILDROOT_EXPECT:-1}" ] ||
    bad "br: ${line:-no buildroot-test summary}"
elif [ "$MODE" = ports ]; then
  # One boot per built port (LINUX_PORTS="lua sqlite ..." picks some);
  # each self-test ends with known lines.
  want() { [ -z "${LINUX_PORTS:-}" ] || case " $LINUX_PORTS " in *" $1 "*) return 0 ;; *) return 1 ;; esac; }
  if want lua && [ -x "$RT/linux-ws/lua-build/lua" ]; then
    echo /usr/share/klausscpu/tests/lua-tests.sh >"$OUT/lua.in"
    printf '%s\n' 'sdinit: data.img mounted read-write at /data' \
      'lua-tests: 19/19 passed' 'lua-selftest: PASS' 'luac lua-selftest: PASS' >"$OUT/lua.expect"
    run_step lua 60000000000 2400 '$ '
  fi
  if want sqlite && [ -x "$RT/linux-ws/sqlite-build/sqlite3" ]; then
    echo /usr/share/klausscpu/tests/sqlite-test.sh >"$OUT/sqlite.in"
    printf '%s\n' 'sdinit: data.img mounted read-write at /data' \
      "sqlite-test: PASS ($(wc -l <"$RT/linux/userland/tests/sqlite-test.expected") lines)" >"$OUT/sqlite.expect"
    run_step sqlite 60000000000 2400 '$ '
  fi
  if want bench && [ -x "$RT/linux-ws/coremark-build/coremark" ] && [ -x "$RT/linux-ws/lmbench-build/tlbmiss" ]; then
    echo '/usr/share/klausscpu/tests/bench.sh quick' >"$OUT/bench.in"
    # CoreMark's CRCs for the performance seeds (a 20-iteration run is too
    # short to score, so CoreMark itself reports "Errors detected").
    printf '%s\n' '[0]crclist       : 0xe714' '[0]crcmatrix     : 0x1fd7' '[0]crcstate      : 0x8e3a' \
      'bench: done' >"$OUT/bench.expect"
    run_step bench 120000000000 3600 '$ '
    # Each benchmark printed a result (the numbers are for the board).
    for re in 'Simple syscall: [0-9.]+ microseconds' 'Simple read: [0-9.]+' 'Simple stat: [0-9.]+' \
              'Process fork\+exit: [0-9.]+' 'Process fork\+execve: [0-9.]+' 'Process fork\+/bin/sh -c: [0-9.]+' \
              '^2 [0-9.]+' 'Pagefaults on /tmp/pf: [0-9.]+' '^rd [0-9.]+ [0-9.]+' '^bcopy [0-9.]+ [0-9.]+' \
              '^0\.0[0-9]+ [0-9.]+' 'tlb: [0-9]+ pages' 'pages +64 +ns/access +[0-9.]+'; do
      grep -aEq "$re" "$OUT/bench.out" || bad "bench: no line matching /$re/"
    done
    grep -aq '\]ERROR!' "$OUT/bench.out" && bad "bench: CoreMark CRC error"
    true
  fi
  if want iperf3 && [ -x "$RT/linux-ws/iperf3-build/iperf3" ]; then
    echo /usr/share/klausscpu/tests/iperf3-test.sh >"$OUT/iperf3.in"
    printf '%s\n' 'iperf3-test: PASS' >"$OUT/iperf3.expect"
    run_step iperf3 60000000000 2400 '$ '
  fi
  if want curl && [ -x "$RT/linux-ws/curl-build/curl" ]; then
    echo /usr/share/klausscpu/tests/curl-test.sh >"$OUT/curl.in"
    printf '%s\n' 'curl-test: 7/7 passed' >"$OUT/curl.expect"
    run_step curl 120000000000 3600 '$ '
  fi
elif [ "$MODE" = keystore ]; then
  # Zephyr's keystore -> Dropbear: kskey reads the synthetic NVS image,
  # dropbearconvert converts the PEM, and Dropbear's fingerprint must be
  # OpenSSH's for the same key.
  cat >"$OUT/keystore.in" <<'EOF2'
kskey /test/storage.img > /tmp/k.pem
dropbearconvert openssh dropbear /tmp/k.pem /tmp/k.db
dropbearkey -y -f /tmp/k.db | grep Fingerprint
EOF2
  printf 'Fingerprint: %s\n' "$KEY_FP" >"$OUT/keystore.expect"
  run_step keystore 20000000000 900 '$ '
elif [ "$MODE" = ssh ]; then
  # Dropbear over loopback: host key, server, a password login with dbclient
  # (the emulator has no Ethernet).  musltest brings lo up.
  cat >"$OUT/ssh.in" <<'EOF2'
/bin/musltest
mkdir -p /etc/dropbear
/usr/bin/dropbearkey -t ed25519 -f /etc/dropbear/dropbear_ed25519_host_key
/usr/sbin/dropbear -r /etc/dropbear/dropbear_ed25519_host_key -p 2222
sleep 1
DROPBEAR_PASSWORD=klausscpu /usr/bin/dbclient -y -p 2222 root@127.0.0.1 'echo ssh-ok; uname -m'
EOF2
  cat >"$OUT/ssh.expect" <<'EOF2'
musltest: PASS
Generating 256 bit ed25519 key, this may take a while...
ssh-ok
klausscpu
EOF2
  run_step ssh 40000000000 1500 '$ '
elif [ "$MODE" = m3 ]; then
  cat >"$OUT/m3.expect" <<'EOF2'
Run /init as init process
init: hello from user space
init: getpid = 1
init: data_word = 1235
init: child sees data_word = 99
init: child exit status 7
init: parent still sees data_word = 1235
init: milestone 3 done, idling
EOF2
  run_step m3 600000000 600
fi

if [ "$MODE" = net ]; then
  IP=${BOARD_IP:-192.168.68.59}
  printf 'sleep 120\nuptime\n' >"$OUT/net.in"
  printf 'eth0: link up\nrcS: ssh on port 22\nrcS: httpd on port 80\n' >"$OUT/net.expect"
  run_step net 0 120 '$ ' &
  step=$!
  timeout 180 bash -c "until grep -aq 'rcS: httpd' '$OUT/net.out' 2>/dev/null; do sleep 2; done" ||
    bad "net: rcS did not start httpd"
  ping -c 3 -W 2 "$IP" >"$OUT/ping.txt" 2>&1 && echo "net: ping ok" || bad "net: ping $IP"
  curl -s -m 10 "http://$IP/" >"$OUT/http.txt" 2>&1
  grep -q 'Linux on KlaussCPU' "$OUT/http.txt" && echo "net: http ok" || bad "net: http page"
  # The SD system image takes key logins only (build-rootfs.sh): the
  # board key when there is one, else the initramfs's password login.
  KEY=${BOARD_SSH_KEY:-$HOME/.ssh/klausscpu_board_ed25519}
  if [ -f "$KEY" ]; then
    timeout 90 ssh -i "$KEY" -o IdentitiesOnly=yes -o BatchMode=yes -o StrictHostKeyChecking=no \
      -o UserKnownHostsFile=/dev/null -o ConnectTimeout=60 "root@$IP" 'uname -nm' </dev/null >"$OUT/ssh.txt" 2>&1
  fi
  if ! grep -q '^klausscpu klausscpu' "$OUT/ssh.txt" 2>/dev/null; then
    SSH_ASKPASS="$RT/tools/validate/zephyr/askpass.sh" SSH_ASKPASS_REQUIRE=force DISPLAY=:0 KLAUSSCPU_SSH_PASS=klausscpu \
      timeout 90 setsid ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
      -o ConnectTimeout=60 -o PreferredAuthentications=password "root@$IP" 'uname -nm' \
      </dev/null >>"$OUT/ssh.txt" 2>&1
  fi
  grep -q '^klausscpu klausscpu' "$OUT/ssh.txt" && echo "net: ssh ok" || bad "net: ssh login"
  wait "$step"
  # run_step ran in the background, so check its console here.
  why=$(expect_in_order "$OUT/net.expect" "$OUT/net.out") || bad "net: $why"
  grep -aq 'Script done' "$OUT/net.out" || bad "net: the board session did not finish"
fi

if [ $fail -eq 0 ]; then echo "linux $MODE $WHERE: PASS"; else echo "linux $MODE $WHERE: $fail failure(s)"; fi
[ $fail -eq 0 ]

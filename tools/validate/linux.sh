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
#                    /proc, tmpfs, and fork+execve of the musl test program
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
#                    (klausscc --sd-native) and the mmc host driver
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
case $MODE in shell|busybox|ssh|net|keystore|rootfs|sd-spi|sd-native|m3) ;; *) die "unknown mode $MODE" ;; esac
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
if [ "$MODE" = shell ] || [ "$MODE" = busybox ] || [ "$MODE" = ssh ] || [ "$MODE" = net ] || [ "$MODE" = keystore ] || [ "$MODE" = rootfs ] || [ $SD = 1 ]; then
  "$RT/linux/musl/build.sh" >"$OUT/musl.log" 2>&1 || { echo "FAIL musl build (see $OUT/musl.log)"; exit 1; }
  export INITRAMFS_PROFILE=minish
fi
if [ "$MODE" = busybox ] || [ "$MODE" = ssh ] || [ "$MODE" = net ] || [ "$MODE" = keystore ] || [ "$MODE" = rootfs ] || [ $SD = 1 ]; then
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
  if [ $SD = 1 ]; then
    # The rootfs image rides in the initramfs as /seed/rootfs.img (not
    # /test, which sdinit would boot); the card starts with an empty FAT32
    # partition at sector 2048, as on the board's card.
    "$RT/linux/rootfs/build-rootfs.sh" "$OUT/rootfs.img" 16 >"$OUT/rootfs.log" 2>&1 ||
      { echo "FAIL rootfs image (see $OUT/rootfs.log)"; exit 1; }
    printf 'dir /seed 0755 0 0\nfile /seed/rootfs.img %s 0644 0 0\n' "$OUT/rootfs.img" >"$OUT/extra.list"
    export INITRAMFS_EXTRA=$OUT/extra.list
    CARD=$OUT/card.img
    truncate -s 64M "$CARD" &&
      echo 'start=2048, type=c' | sfdisk -q "$CARD" >"$OUT/card.log" 2>&1 &&
      mkfs.vfat -F 32 -s 1 -n KLAUSSCPU --offset 2048 "$CARD" $(( 65536 - 1024 )) >>"$OUT/card.log" 2>&1 ||
      { echo "FAIL card image (see $OUT/card.log)"; exit 1; }
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
# prefix "$ " ignored), in order.
expect_in_order() {
  awk 'BEGIN { n = 0; i = 0 }
       NR == FNR { sub(/[ \t]+$/, ""); want[n++] = $0; next }
       { sub(/\r$/, ""); sub(/[ \t]+$/, ""); line = $0
         sub(/^\[ *[0-9]+\.[0-9]+\] /, "", line); while (sub(/^\$ /, "", line)) {}
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
musltest: 300 forks, 0 bad
musltest: PASS
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
sdinit: switching to the SD card's root file system
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
    fi
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
    fi
  } >"$OUT/sd1.expect"
  run_step sd1 100000000000 3000 '$ '
  # The raw read of the card's first 8 MiB, against the image file.
  RAW=$(head -c 8388608 "$CARD" | sha256sum | cut -c1-16)
  grep -aq "$RAW" "$OUT/sd1.out" || bad "sd1: the raw read of mmcblk0 differs from the card image ($RAW)"
  dd if="$CARD" of="$OUT/part.img" bs=512 skip=2048 status=none &&
    fsck.fat -n "$OUT/part.img" >"$OUT/fsck.log" 2>&1 || bad "sd1: fsck.fat on the card's partition (see $OUT/fsck.log)"
  rm -f "$OUT/part.img"

  # Boot 2: sdinit switches to the card's image.
  cat >"$OUT/sd2.in" <<'EOF2'
cut -d' ' -f1 /etc/klausscpu-rootfs
grep ' / ' /proc/mounts | cut -d' ' -f1,3
ls /mnt/linux/rootfs.img
echo persist > /root/t; sync; cat /root/t
/usr/bin/musltest | tail -1
EOF2
  cat >"$OUT/sd2.expect" <<'EOF2'
sdinit: switching to the SD card's root file system
klausscpu-rootfs
/dev/loop0 ext4
/mnt/linux/rootfs.img
persist
musltest: PASS
EOF2
  run_step sd2 60000000000 2400 '$ '
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
  printf 'eth0: link up\nrcS: dropbear on port 22\nrcS: httpd on port 80\n' >"$OUT/net.expect"
  run_step net 0 120 '$ ' &
  step=$!
  timeout 180 bash -c "until grep -aq 'rcS: httpd' '$OUT/net.out' 2>/dev/null; do sleep 2; done" ||
    bad "net: rcS did not start httpd"
  ping -c 3 -W 2 "$IP" >"$OUT/ping.txt" 2>&1 && echo "net: ping ok" || bad "net: ping $IP"
  curl -s -m 10 "http://$IP/" >"$OUT/http.txt" 2>&1
  grep -q 'Linux on KlaussCPU' "$OUT/http.txt" && echo "net: http ok" || bad "net: http page"
  SSH_ASKPASS="$RT/tools/validate/zephyr/askpass.sh" SSH_ASKPASS_REQUIRE=force DISPLAY=:0 KLAUSSCPU_SSH_PASS=klausscpu \
    timeout 90 setsid ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -o ConnectTimeout=60 -o PreferredAuthentications=password "root@$IP" 'uname -nm' \
    </dev/null >"$OUT/ssh.txt" 2>&1
  grep -q '^klausscpu klausscpu' "$OUT/ssh.txt" && echo "net: ssh ok" || bad "net: ssh login"
  wait "$step"
  # run_step ran in the background, so check its console here.
  why=$(expect_in_order "$OUT/net.expect" "$OUT/net.out") || bad "net: $why"
  grep -aq 'Script done' "$OUT/net.out" || bad "net: the board session did not finish"
fi

if [ $fail -eq 0 ]; then echo "linux $MODE $WHERE: PASS"; else echo "linux $MODE $WHERE: $fail failure(s)"; fi
[ $fail -eq 0 ]

#!/usr/bin/env bash
# linux/buildroot/push.sh [-n] [-p] PKG...: add or update Buildroot packages
# on the running board without a new system image or a reboot.
#
# Builds PKG (the shared build, linux-ws/br-output-dyn; a package not yet
# selected is switched on in its .config first) and its dependencies, takes
# the files Buildroot installed for them (build/<pkg>/.files-list.txt), as
# build-rootfs.sh would merge them into the image (bin, sbin, usr, lib; no
# headers, static libraries, pkg-config, man or doc files, no /etc), leaves
# out files identical to the installed image's (linux-ws/rootfs-staging,
# from the last build-rootfs.sh: the image the board runs), applies the
# packages' setuid/permission entries, and streams the rest onto the board
# with tar.  The board's root must be sdinit's overlay (`overlay status`):
# the files land in /data/overlay/upper and last until the next system
# image, which starts clean (or `overlay reset`).
#   -n   list what would be pushed, push nothing
#   -p   also make the package part of the image from now on: add it to
#        external/configs/klausscpu_defconfig (then rebuild the image as
#        usual when convenient)
#   BOARD_SSH, BOARD_SSH_KEY, BOARD_IP  as tools/validate/board-check.sh
# Take the board first (the board protocol): this script does not lock.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../.." && pwd)
WS=$(realpath -m "${LINUX_WS:-$RT/linux-ws}")
BO=$WS/br-output-dyn
IMG=$WS/rootfs-staging
DRY=0 PERM=0
while [ $# -gt 0 ]; do
	case $1 in
	-n) DRY=1 ;;
	-p) PERM=1 ;;
	-*) echo "usage: push.sh [-n] [-p] PKG..." >&2; exit 2 ;;
	*) break ;;
	esac
	shift
done
[ $# -ge 1 ] || { echo "usage: push.sh [-n] [-p] PKG..." >&2; exit 2; }
IP=${BOARD_IP:-192.168.68.59}
KEY=${BOARD_SSH_KEY:-$HOME/.ssh/klausscpu_board_ed25519}
KO="-o UserKnownHostsFile=$HOME/.cache/klausscpu-board-known_hosts -o StrictHostKeyChecking=accept-new"
[ -f "$KEY" ] && KO="$KO -i $KEY -o IdentitiesOnly=yes"
read -r -a SSH <<<"${BOARD_SSH:-ssh $KO -o BatchMode=yes -o ConnectTimeout=20 root@$IP}"
[ -d "$IMG" ] || { echo "no $IMG: build the image once (linux/rootfs/build-rootfs.sh)" >&2; exit 2; }
# The .config names the toolchain as $(KLAUSSCPU_WS)/br-toolchain-dyn (build.sh).
export KLAUSSCPU_WS=$WS
MK=(make -s -C "$WS/buildroot" O="$BO" BR2_EXTERNAL="$HERE/external")
up() { echo "$1" | tr 'a-z-' 'A-Z_'; }

# Switch on packages that aren't selected yet (and keep what they select);
# a failed build puts the .config back.
cp "$BO/.config" "$BO/.config.push-backup"
changed=0
for p in "$@"; do
	v=BR2_PACKAGE_$(up "$p")
	if ! grep -q "^$v=y" "$BO/.config"; then
		grep -q "^# $v is not set\|^$v=" "$BO/.config" || [ -d "$WS/buildroot/package/$p" ] ||
			{ echo "push.sh: no Buildroot package $p" >&2; exit 2; }
		sed -i "/^# $v is not set/d; /^$v=/d" "$BO/.config"
		echo "$v=y" >>"$BO/.config"
		changed=1
	fi
	if [ $PERM = 1 ] && ! grep -q "^$v=y" "$HERE/external/configs/klausscpu_defconfig"; then
		echo "$v=y" >>"$HERE/external/configs/klausscpu_defconfig"
		echo "push.sh: $v=y added to klausscpu_defconfig"
	fi
done
[ $changed = 1 ] && "${MK[@]}" olddefconfig >/dev/null
for p in "$@"; do
	grep -q "^BR2_PACKAGE_$(up "$p")=y" "$BO/.config" ||
		{ echo "push.sh: $p can't be selected (a dependency that is off?)" >&2; exit 2; }
done

# Build them; then the packages to take files from: them and every
# package they depend on.
if ! BR_SHARED=1 "$HERE/build.sh" "$@" >"$WS/push-build.log" 2>&1; then
	tail -20 "$WS/push-build.log"
	if [ $changed = 1 ]; then
		cp "$BO/.config.push-backup" "$BO/.config"
		"${MK[@]}" olddefconfig >/dev/null
		echo "push.sh: the .config is as it was" >&2
	fi
	echo "push.sh: build failed (see $WS/push-build.log)" >&2
	exit 1
fi
# (not the toolchain or skeleton: the C library and the loader are the
# image's), and the files each installed (build/<pkg>/.files-list.txt,
# written as it installs; packages-file-list.txt is only gathered by a
# whole build).
PKGS=$(for p in "$@"; do echo "$p"; "${MK[@]}" "$p-show-recursive-depends" 2>/dev/null | tr ' ' '\n'; done |
	grep -v '^host-\|^toolchain\|^skeleton\|^musl-compat-headers$\|^$' | sort -u)
FL=$(mktemp)
cat "$BO"/build/*/.files-list.txt >"$FL"

ST=$(mktemp -d "${XDG_CACHE_HOME:-$HOME/.cache}/br-push.XXXXXX")
trap 'rm -rf "$ST" "$FL"' EXIT
mkdir -p "$ST/root"
n=0
for p in $PKGS; do
	{ grep "^$p," "$FL" || true; } | cut -d, -f2- | sed 's|^\./||'
done | sort -u | while IFS= read -r f; do
	case $f in bin/*|sbin/*|usr/*|lib/*) ;; *) continue ;; esac
	case $f in usr/include/*|*.a|*.la|usr/lib/pkgconfig/*|usr/share/aclocal/*|usr/share/man/*|usr/share/doc/*) continue ;; esac
	src=$BO/target/$f
	[ -e "$src" ] || [ -L "$src" ] || continue
	# The same as in the image: nothing to push.
	if [ -L "$src" ]; then
		[ -L "$IMG/$f" ] && [ "$(readlink "$src")" = "$(readlink "$IMG/$f")" ] && continue
	elif [ -f "$src" ]; then
		[ -f "$IMG/$f" ] && [ ! -L "$IMG/$f" ] && cmp -s "$src" "$IMG/$f" && continue
	else
		continue
	fi
	mkdir -p "$ST/root/$(dirname "$f")"
	cp -a "$src" "$ST/root/$f"
	echo "$f"
done >"$ST/list"
# The packages' permission entries (setuid sudo and the like).
for p in $PKGS; do
	"${MK[@]}" printvars VARS="$(up "$p")_PERMISSIONS" RAW_VARS=YES QUOTED_VARS=NO 2>/dev/null |
		sed "s/^[A-Z0-9_]*_PERMISSIONS=//; s/'//g"
done | awk 'NF >= 3 && $2 == "f" {print $1, $3}' | while read -r path mode; do
	[ -f "$ST/root$path" ] && chmod "$mode" "$ST/root$path" && echo "push.sh: $path mode $mode"
done
n=$(wc -l <"$ST/list")
echo "push.sh: $(echo $PKGS | wc -w) package(s) ($*, with dependencies): $n file(s) differ from the image"
sed 's|^|  /|' "$ST/list"
[ "$n" -gt 0 ] || exit 0
[ $DRY = 1 ] && exit 0
"${SSH[@]}" "grep -q '^overlay / ' /proc/mounts" ||
	{ echo "push.sh: the board's root is not the overlay (overlay status): nothing pushed" >&2; exit 1; }
tar -C "$ST/root" --owner=0 --group=0 --numeric-owner -czf - . | "${SSH[@]}" 'tar -xzf - -C / && sync'
echo "push.sh: pushed to the board (overlay files lists them; overlay reset or a new image removes them)"

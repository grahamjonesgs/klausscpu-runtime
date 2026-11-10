#!/usr/bin/env bash
# linux/buildroot/toolchain.sh — the external toolchain Buildroot uses
# (BR2_TOOLCHAIN_EXTERNAL_CUSTOM), in linux-ws/br-toolchain:
#   bin/klausscpu-linux-musl-{gcc,cc,cpp,as,ld,ar,ranlib,nm,objcopy,objdump,
#                             readelf,strip,size,strings,addr2line,c++filt,
#                             gcc-ar,gcc-nm,gcc-ranlib}
#   sysroot/  linux/musl's sysroot in the usual layout (usr/include,
#             usr/lib; include and lib link there) with the 7.3 kernel
#             headers, as Buildroot's sysroot discovery and copy expect.
# The compiler wrappers run KLAUSSCPU_LLVM_BIN's clang for
# klausscpu-unknown-linux-musl and make it look enough like gcc:
#   - -dumpversion / -dumpfullversion answer 14.2.0: the defconfig selects
#     GCC 14 (clang 23 takes what GCC 14 does) and Buildroot checks it;
#   - -msoft-float is dropped (KlaussCPU code is always soft-float; clang
#     only warns, which -Werror packages would trip on);
#   - for --sysroot=X (Buildroot's staging) they also search X/usr/include
#     and X/usr/lib, where packages install, besides clang's X/include and
#     X/lib, after the package's own directories, as gcc does;
#   - always -static (musl, static only), and -ffunction-sections
#     -fdata-sections: a .text bigger than the branch range can't get
#     range-extension thunks inside it (SQLite hit this).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../.." && pwd)
WS=${LINUX_WS:-$RT/linux-ws}
B=$(realpath "${KLAUSSCPU_LLVM_BIN:-$RT/../klausscpu-llvm/build/bin}")
OUT=$WS/br-toolchain
P=klausscpu-linux-musl
[ -f "$WS/sysroot/lib/libc.a" ] || { echo "no musl sysroot: run linux/musl/build.sh" >&2; exit 1; }
[ -f "$WS/sysroot/include/linux/version.h" ] || { echo "no kernel headers in the musl sysroot" >&2; exit 1; }
rm -rf "$OUT"; mkdir -p "$OUT/bin" "$OUT/sysroot/usr"
cp -a "$WS/sysroot/include" "$OUT/sysroot/usr/include"
cp -a "$WS/sysroot/lib" "$OUT/sysroot/usr/lib"
ln -s usr/include "$OUT/sysroot/include"
ln -s usr/lib "$OUT/sysroot/lib"

cat >"$OUT/bin/$P-gcc" <<EOF
#!/bin/bash
# $P-gcc: clang as a gcc for Buildroot (linux/buildroot/toolchain.sh).
mode=\${0##*-}
sysroot="$OUT/sysroot"
args=()
next_sysroot=0
for a in "\$@"; do
	if [ \$next_sysroot = 1 ]; then sysroot=\$a; next_sysroot=0; continue; fi
	case "\$a" in
	-dumpversion|-dumpfullversion) echo 14.2.0; exit 0 ;;
	-msoft-float) ;;
	--sysroot=*) sysroot=\${a#--sysroot=} ;;
	--sysroot) next_sysroot=1 ;;
	*) args+=("\$a") ;;
	esac
done
pre=(--target=klausscpu-unknown-linux-musl --sysroot="\$sysroot")
[ "\$mode" = cpp ] && exec "$B/clang" "\${pre[@]}" -idirafter "\$sysroot/usr/include" -E "\${args[@]}"
[ "\$mode" = as ] && exec "$B/clang" "\${pre[@]}" -c -x assembler "\${args[@]}"
# The sysroot's directories come last, as gcc's do: -idirafter after every
# -I/-isystem (strace's bundled kernel headers must win over the sysroot's
# newer ones), and -L after the package's own (its freshly built libraries
# must win over what staging holds).
exec "$B/clang" "\${pre[@]}" -idirafter "\$sysroot/usr/include" \\
	-static -ffunction-sections -fdata-sections -Wno-unused-command-line-argument \\
	"\${args[@]}" -L"\$sysroot/usr/lib"
EOF
chmod +x "$OUT/bin/$P-gcc"
for m in cc cpp as; do ln -s "$P-gcc" "$OUT/bin/$P-$m"; done
ln -s "$B/ld.lld" "$OUT/bin/$P-ld"
for t in ar ranlib nm objcopy objdump readelf strip size strings addr2line; do
  ln -s "$B/llvm-$t" "$OUT/bin/$P-$t"
done
ln -s "$B/llvm-cxxfilt" "$OUT/bin/$P-c++filt"
# gcc's LTO-aware wrappers, which some packages (zlib) call by name.
for t in ar nm ranlib; do ln -s "$B/llvm-$t" "$OUT/bin/$P-gcc-$t"; done
echo "br-toolchain: $OUT ($("$OUT/bin/$P-gcc" --version | head -1))"

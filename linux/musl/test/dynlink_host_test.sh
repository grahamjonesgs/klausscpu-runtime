#!/usr/bin/env bash
# linux/musl/test/dynlink_host_test.sh MUSL_SRC LIB.so — run the patched musl
# dynamic linker's ELF32-on-LP64 code (decode, hash lookups, relocation) on
# the host against a KlaussCPU shared object (dynlink_host_test.c says what
# it checks).  MUSL_SRC is a musl checkout with linux/musl/patches applied
# (build.sh does that); LIB.so e.g. its lib/libc.so from MUSL_SHARED=1.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$1" && pwd)
LIB=$2
if [ -n "${KEEP:-}" ]; then   # KEEP=dir: build there and keep it
  OUT=$KEEP; mkdir -p "$OUT"
else
  OUT=$(mktemp -d "${TMPDIR:-/tmp}/dynlink-host-test.XXXXXX")
  trap 'rm -rf "$OUT"' EXIT
fi
D=$SRC/ldso/dynlink.c
grep -q 'decode_dynv' "$D" || { echo "$D: the patches are not applied"; exit 1; }
# The code under test, cut out of dynlink.c unchanged: struct dso and its
# companions, the loader's globals (ldso, head, the saved addends),
# dl_strcmp, laddr .. do_relocs, decode_dyn and count_syms.
cut_range() { # cut_range FIRST-LINE-REGEX STOP-LINE-REGEX
  awk -v a="$1" -v b="$2" '$0 ~ a {on=1} on && $0 ~ b {exit} on' "$D"
}
{
  cut_range '^struct td_index \{' '^typedef void \(\*stage3_func\)'
  cut_range '^#define ADDEND_LIMIT' '^static char \*env_path'
  cut_range '^static int dl_strcmp' '^/\* Compute load address'
  cut_range '^/\* Compute load address' '^static void do_relr_relocs\('
  cut_range '^static void decode_dyn\(struct dso \*p\)' '^static void \*dl_mmap\('
} >"$OUT/dynlink_parts.h"
# Unused helpers (decode_vec and search_vec are kept for completeness).
cc -std=gnu11 -O1 -g -Wall -Wno-unused-function -Wno-unused-variable \
  -I"$OUT" -I"$SRC/src/internal" -I"$HERE/../arch/klausscpu" \
  -o "$OUT/dynlink_host_test" "$HERE/dynlink_host_test.c"
"$OUT/dynlink_host_test" "$LIB"

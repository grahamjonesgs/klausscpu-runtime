#!/usr/bin/env bash
# oracle_diff.sh <base> <new> -- compare two oracle.sh snapshots: report every
# file that differs, is missing or is new (logs excepted).  Exit 1 if any.
set -uo pipefail
[ $# -eq 2 ] || { echo "usage: oracle_diff.sh <base> <new>" >&2; exit 2; }
A=$(realpath "$1"); N=$(realpath "$2"); rc=0
while IFS= read -r f; do
  if [ ! -e "$N/$f" ]; then echo "MISSING $f"; rc=1
  elif ! cmp -s "$A/$f" "$N/$f"; then echo "DIFF $f"; rc=1; fi
done < <(cd "$A" && find . -type f ! -name '*.log' ! -name '*.err' | sort)
while IFS= read -r f; do
  [ -e "$A/$f" ] || { echo "NEW $f"; rc=1; }
done < <(cd "$N" && find . -type f ! -name '*.log' ! -name '*.err' | sort)
[ $rc = 0 ] && echo "ORACLE IDENTICAL ($(cd "$A" && find . -type f ! -name '*.log' ! -name '*.err' | wc -l) files)"
exit $rc

#!/usr/bin/env bash
# test_kskey.sh <workdir>: host unit tests for kskey against NVS images
# built by mknvs.py to Zephyr's format, and the key's fingerprint through
# OpenSSH's ssh-keygen.  Exits 1 on any failure.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
W=$1
mkdir -p "$W"
gcc -O2 -Wall -Wextra -Werror -o "$W/kskey" "$HERE/kskey.c" || exit 1
openssl ecparam -name prime256v1 -genkey -noout -outform DER -out "$W/key.der" 2>/dev/null || exit 1
openssl ec -inform DER -in "$W/key.der" -out "$W/key.pem" 2>/dev/null || exit 1
fail=0
for s in newest torn rollover wrap; do
  python3 -I "$HERE/mknvs.py" "$W/$s.img" "$s" "$W/key.der" || exit 1
  if "$W/kskey" -d "$W/$s.img" | cmp -s - "$W/key.der"; then echo "PASS $s"
  else echo "FAIL $s"; fail=1; fi
done
for s in deleted badcrc; do
  python3 -I "$HERE/mknvs.py" "$W/$s.img" "$s" "$W/key.der" || exit 1
  if "$W/kskey" "$W/$s.img" >/dev/null 2>&1; then echo "FAIL $s (found a key)"; fail=1
  else echo "PASS $s (no key)"; fi
done
# The PEM kskey prints is the same key to OpenSSH as the original.
"$W/kskey" "$W/newest.img" >"$W/out.pem" && chmod 600 "$W/out.pem" "$W/key.pem"
want=$(ssh-keygen -y -f "$W/key.pem" | ssh-keygen -lf - | awk '{print $2}')
got=$(ssh-keygen -y -f "$W/out.pem" | ssh-keygen -lf - | awk '{print $2}')
if [ -n "$got" ] && [ "$got" = "$want" ]; then echo "PASS fingerprint $got"
else echo "FAIL fingerprint: got '$got', want '$want'"; fail=1; fi
[ $fail -eq 0 ] && echo "kskey: all tests PASS"
exit $fail

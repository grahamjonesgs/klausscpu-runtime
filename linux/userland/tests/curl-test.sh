#!/bin/sh
# curl-test.sh: curl without the network.  file://, then HTTP on loopback
# against busybox httpd (a page, a HEAD request, a 1 MiB download checked by
# SHA-256), the CA bundle, and mbedTLS's own self-test (the crypto under
# https).  Prints one line per check and "curl-test: N/M passed".
# Installed as /usr/share/klausscpu/tests/curl-test.sh.  On the board:
#   curl -sS https://example.com/ | head
n=0
pass=0
check() { # <name> <command...>: passes when the command succeeds
	n=$((n + 1))
	name=$1
	shift
	if "$@" >/dev/null 2>&1; then
		pass=$((pass + 1))
		echo "curl-test $name: OK"
	else
		echo "curl-test $name: FAIL"
	fi
}
curl --version | head -1
check file sh -c 'curl -sS file:///etc/klausscpu-rootfs | cmp -s - /etc/klausscpu-rootfs'
mkdir -p /tmp/www
cp /etc/klausscpu-rootfs /tmp/www/index.html
head -c 1048576 /usr/bin/curl >/tmp/www/big.bin
httpd -p 127.0.0.1:8080 -h /tmp/www
sleep 1
check http-page sh -c 'curl -sS http://127.0.0.1:8080/ | cmp -s - /tmp/www/index.html'
check http-head sh -c 'curl -sSI http://127.0.0.1:8080/big.bin | grep -q "^HTTP/1.[01] 200"'
check http-1MiB sh -c '[ "$(curl -sS http://127.0.0.1:8080/big.bin | sha256sum)" = "$(sha256sum </tmp/www/big.bin)" ]'
check http-404 sh -c '[ "$(curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:8080/none)" = 404 ]'
check ca-bundle sh -c '[ "$(grep -c "BEGIN CERTIFICATE" /etc/ssl/certs/ca-certificates.crt)" -gt 100 ]'
check mbedtls-selftest /usr/libexec/mbedtls-selftest
killall httpd 2>/dev/null
echo "curl-test: $pass/$n passed"

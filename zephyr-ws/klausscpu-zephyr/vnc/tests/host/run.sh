#!/bin/sh
# Host-side VNC server test: compiles the REAL ../../vnc_server.c against stub
# Zephyr headers and fuzzes the Hextile encoder + zero-copy send paths.
# Runs on any dev host with gcc/clang — no Zephyr SDK needed.
set -e
cd "$(dirname "$0")"
CC="${CC:-gcc}"
for extra in "" "-DCONFIG_KLAUSSCPU_VNC_PROFILE=1" \
	     "-DCONFIG_KLAUSSCPU_VNC_PROFILE=1 -DCONFIG_KLAUSSCPU_VNC_BLIT_SEND=1"; do
	echo "== build: ${extra:-defaults} =="
	$CC -std=c11 -Wall -Wextra -Wno-unused-parameter -O1 $extra \
	    -I . -I ../.. test_hextile.c -o test_hextile_bin
	./test_hextile_bin
done
rm -f test_hextile_bin

#!/bin/sh
# lua-tests.sh: Lua's own tests (the files that run standalone), in their
# portable, soft mode, then lua-selftest.lua.  Prints one line per file
# and "lua-tests: N/M passed".  Installed in the system image as
# /usr/share/klausscpu/tests/lua-tests.sh.
D=$(dirname "$0")
# musl's setlocale accepts any name but keeps the C number format, so
# literals.lua's "decimal point locale" block would believe pt_BR is in
# effect: say no to locales other than C/POSIX, as a C library without
# them does.
PRE='_soft=true; _port=true; local sl = os.setlocale; os.setlocale = function(l, ...) if l and l ~= "" and l ~= "C" and l ~= "POSIX" then return nil end return sl(l, ...) end'
cd "$D/lua" || exit 1
n=0
pass=0
for t in strings math closure coroutine nextvar sort events calls constructs \
	vararg tpack utf8 bitwise literals goto pm errors db attrib; do
	n=$((n + 1))
	if lua -e "$PRE" "$t.lua" >/tmp/lua-$t.out 2>&1 &&
	   tail -1 /tmp/lua-$t.out | grep -qi '^ok$'; then
		pass=$((pass + 1))
		echo "lua-test $t: OK"
	else
		echo "lua-test $t: FAIL"
		tail -5 /tmp/lua-$t.out
	fi
done
echo "lua-tests: $pass/$n passed"
lua "$D/lua-selftest.lua"
luac -o /tmp/luac.out "$D/lua-selftest.lua" && lua /tmp/luac.out | tail -1 | sed 's/^/luac /'

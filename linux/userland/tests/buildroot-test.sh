#!/bin/sh
# buildroot-test.sh: a check of each Buildroot package in the system image
# (linux/buildroot, merged by build-rootfs.sh with ROOTFS_BUILDROOT=1):
# its --version, and a small real use.  Packages that aren't installed are
# listed as such.  The commands call each program as "$P", its real path:
# the busybox shell runs its own applets in preference to programs of the
# same name (FEATURE_PREFER_APPLETS), as with less.  Prints
# "br-test <pkg>: OK|FAIL ..." and
# "buildroot-test: N/M passed".  Installed as
# /usr/share/klausscpu/tests/buildroot-test.sh.
n=0
pass=0
t() { # <name> <program> <shell command using $P>: the command must succeed
	P=
	for d in /usr/bin /usr/sbin /bin /sbin; do
		if [ -f "$d/$2" ] && ! { [ -L "$d/$2" ] && readlink "$d/$2" | grep -q busybox; }; then
			P=$d/$2
			break
		fi
	done
	if [ -z "$P" ]; then
		echo "br-test $1: not installed"
		return
	fi
	export P
	n=$((n + 1))
	if out=$(sh -c "$3" 2>&1); then
		pass=$((pass + 1))
		echo "br-test $1: OK $(echo "$out" | tail -1)"
	else
		echo "br-test $1: FAIL"
		echo "$out" | tail -4
	fi
}
cd /tmp || exit 1
export TERM=${TERM:-vt100}
t nano nano '"$P" --version | head -1 | grep "GNU nano"'
t ncurses tput '/usr/bin/infocmp -1 vt100 | grep -q "cols#80," && "$P" -T vt100 clear | od -c | grep -q 033 && echo "terminfo vt100: cols#80, clear ok"'
t zlib file 'echo "hello zlib" | gzip >z.gz && "$P" -z z.gz | grep -q "ASCII text" && "$P" -z z.gz'
t file file '"$P" /bin/busybox | grep -q ELF && "$P" /usr/share/klausscpu/tests/bench.sh'
t less less '"$P" --version | head -1 | grep -q "^less [0-9]" && printf "one\ntwo\n" | "$P" -F -X | tail -1 | grep -qx two && "$P" --version | head -1'
t tree tree 'mkdir -p tr/a/b && touch tr/a/f tr/a/b/g && out=$("$P" tr) && echo "$out" | tail -1 | grep -q "directories, 2 files" && echo "$out" | tail -1'
t jq jq '[ "$(echo "{\"a\":[1,2,3]}" | "$P" -c ".a|map(.*2)")" = "[2,4,6]" ] && "$P" --version'
t htop htop '"$P" --version | head -1'
t tmux tmux '"$P" -S /tmp/tm new -d -s t "sleep 3" && "$P" -S /tmp/tm ls | grep -q "^t:" && "$P" -S /tmp/tm kill-server && "$P" -V'
t strace strace '"$P" -f -o st1 /bin/true && grep -q "execve(\"/bin/true\"" st1 && grep -q "exit_group(0)" st1 &&
	"$P" -o st2 ls / >/dev/null && grep -q "getdents64(" st2 && grep -Eq "writev?\(1, " st2 &&
	{ "$P" -o st3 -e trace=openat cat /nonexistent 2>/dev/null; grep -q "openat(.*\"/nonexistent\".* = -1 ENOENT" st3; } &&
	{ "$P" -o st4 -e trace=uname -e inject=uname:error=EPERM uname >/dev/null 2>&1; grep -q "= -1 EPERM .*(INJECTED)" st4; } &&
	echo "strace -f /bin/true:" && cat st1 && grep -h -E "ENOENT|INJECTED" st3 st4 && "$P" -V | head -1'
echo "buildroot-test: $pass/$n passed"

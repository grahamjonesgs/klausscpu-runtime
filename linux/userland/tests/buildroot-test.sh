#!/bin/sh
# buildroot-test.sh: a check of each Buildroot package in the system image
# (linux/buildroot, merged by build-rootfs.sh with ROOTFS_BUILDROOT=1):
# its --version, and a small real use.  Packages that aren't installed are
# listed as such.  Prints "br-test <pkg>: OK|FAIL ..." and
# "buildroot-test: N/M passed".  Installed as
# /usr/share/klausscpu/tests/buildroot-test.sh.
n=0
pass=0
t() { # <name> <program> <shell command>: the command must succeed
	if ! command -v "$2" >/dev/null 2>&1; then
		echo "br-test $1: not installed"
		return
	fi
	case $(readlink -f "$(command -v "$2")") in
	*/busybox) echo "br-test $1: not installed (busybox applet)"; return ;;
	esac
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
t nano nano 'nano --version | head -1 | grep "GNU nano"'
t ncurses tput 'c=$(tput -T vt100 cols); echo "tput cols: $c"; [ "$c" = 80 ] && infocmp -1 vt100 | grep -q "clear=" && echo "terminfo vt100 ok"'
t zlib file 'echo "hello zlib" | gzip >z.gz && file -z z.gz | grep -q "ASCII text" && file -z z.gz'
t file file 'file /bin/busybox | grep -q ELF && file /usr/share/klausscpu/tests/bench.sh'
t less less 'less --version | head -1 | grep -q "^less [0-9]" && printf "one\ntwo\n" | less -F -X | tail -1 | grep -qx two && less --version | head -1'
t tree tree 'mkdir -p tr/a/b && touch tr/a/f tr/a/b/g && tree tr | tail -1 | grep -q "2 directories, 2 files" && tree --version | head -1'
t jq jq '[ "$(echo "{\"a\":[1,2,3]}" | jq -c ".a|map(.*2)")" = "[2,4,6]" ] && jq --version'
t htop htop 'htop --version | head -1'
t tmux tmux 'tmux -S /tmp/tm new -d -s t "sleep 3" && tmux -S /tmp/tm ls | grep -q "^t:" && tmux -S /tmp/tm kill-server && tmux -V'
t strace strace 'strace -f -o /tmp/st.out true && grep -q execve /tmp/st.out && strace -V | head -1'
echo "buildroot-test: $pass/$n passed"

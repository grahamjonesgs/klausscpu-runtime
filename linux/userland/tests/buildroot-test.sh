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
# Development and test tools.
t bash bash '[ "$("$P" -c "a=(1 2 3); echo \${#a[@]} \$((2**10))")" = "3 1024" ] && "$P" --version | head -1'
t bc bc '[ "$(echo "scale=10; 4*a(1)" | "$P" -l)" = "3.1415926532" ] && echo "pi = $(echo "scale=10; 4*a(1)" | "$P" -l)"'
t gawk gawk '[ "$(echo "a b c" | "$P" "{ print NF, toupper(\$2) }")" = "3 B" ] && "$P" --version | head -1'
t diffutils diff 'printf "a\nb\n" >d1 && printf "a\nc\n" >d2 && { "$P" -u d1 d2 >d.out; [ $? = 1 ]; } && grep -q "^@@ -1,2 +1,2 @@" d.out && /usr/bin/cmp d1 d1 && "$P" --version | head -1'
t make make 'printf "all: m.out\nm.out:\n\techo built > m.out\n" >Makefile.t && rm -f m.out && "$P" -s -f Makefile.t && grep -qx built m.out && "$P" --version | head -1'
t vim vim '"$P" -es -N -u NONE -c "call setline(1, \"hello vim\")" -c "wq! /tmp/v.txt" && grep -qx "hello vim" /tmp/v.txt && "$P" --version | head -1'
t ncdu ncdu '"$P" -v'
t dos2unix dos2unix 'printf "a\r\nb\r\n" >dos.txt && "$P" -q dos.txt && ! grep -q "$(printf "\r")" dos.txt && "$P" --version | head -1'
t xz xz 'head -c 100000 /bin/busybox >c.in && "$P" -c c.in | "$P" -dc | cmp - c.in && "$P" --version | head -1'
t bzip2 bzip2 'head -c 100000 /bin/busybox >c.in && "$P" -c c.in | "$P" -dc | cmp - c.in && echo "bzip2 round trip ok"'
t zstd zstd 'head -c 100000 /bin/busybox >c.in && "$P" -q -c c.in | "$P" -q -dc | cmp - c.in && "$P" --version'
t rsync rsync 'rm -rf rs1 rs2 && mkdir -p rs1/d && echo x >rs1/d/f && ln -s d/f rs1/l && "$P" -a rs1/ rs2/ && [ -L rs2/l ] && cmp rs1/d/f rs2/d/f && "$P" --version | head -1'
t e2fsprogs mke2fs 'rm -f e2.img && head -c 4194304 /dev/zero >e2.img && "$P" -q -F -t ext4 e2.img && /sbin/e2fsck -fn e2.img >/dev/null && /sbin/debugfs -R "ls -l /" e2.img 2>/dev/null | grep -q lost+found && echo "mke2fs, e2fsck, debugfs ok"'
t dosfstools mkfs.fat 'rm -f fat.img && head -c 4194304 /dev/zero >fat.img && "$P" fat.img >/dev/null && /sbin/fsck.fat -n fat.img | tail -1'
t psmisc pstree '"$P" -p 1 | head -1 | grep -q "init(1)" && /usr/bin/killall -V 2>&1 | head -1'
t lsof lsof '"$P" -p $$ | grep -q " cwd " && "$P" -v 2>&1 | grep -i "revision" | head -1'
t sysstat iostat '"$P" -d 1 1 | grep -q "Device" && /usr/bin/mpstat 1 1 | grep -q "all" && "$P" -V | head -1'
t iproute2 ss '/sbin/ip -br link | grep -q "^lo " && "$P" -tln | grep -q ":22 " && echo "ss: sshd on :22"'
t tcpdump tcpdump '( sleep 1; ping -c 3 127.0.0.1 >/dev/null ) & "$P" -i lo -n -c 2 icmp 2>/dev/null | grep -c "ICMP echo" | grep -qx 2 && "$P" --version 2>&1 | head -1'
t socat socat '"$P" TCP-LISTEN:7777,reuseaddr SYSTEM:cat & sleep 1; [ "$(echo hello | "$P" - TCP:127.0.0.1:7777)" = hello ] && "$P" -V | grep "socat version"'
t ethtool ethtool '"$P" --version'
t stress-ng stress-ng '"$P" --cpu 1 --cpu-ops 200 --vm 1 --vm-bytes 1M --vm-ops 50 --timeout 60s 2>&1 | grep "successful run completed"'
t memtester memtester '"$P" 1M 1 | grep -q "Done" && echo "memtester 1M: Done"'
t dhrystone dhrystone 'echo 3000000 | "$P" | grep -i "Dhrystones per Second"'
t whetstone whetstone '"$P" 2000 | tail -1'
t fio fio '"$P" --name=t --filename=/tmp/fio.dat --size=4M --rw=randrw --bs=4k --ioengine=psync --runtime=5 --verify=crc32c --do_verify=1 >fio.out 2>&1; grep -q "err= 0" fio.out && rm -f /tmp/fio.dat && "$P" --version'
t git git 'rm -rf gt && mkdir gt && cd gt && "$P" init -q && echo hi >f && "$P" add f && "$P" -c user.name=t -c user.email=t@e commit -q -m first && [ "$("$P" log --format=%s)" = first ] && "$P" --version'
t openssh ssh-keygen 'rm -f k k.pub && "$P" -q -t ed25519 -N "" -f k && grep -q "^ssh-ed25519 " k.pub && [ -x /usr/libexec/sftp-server ] && /usr/bin/ssh -V 2>&1'
t util-linux lsblk '"$P" -n -o NAME | grep -q mmcblk0 && /bin/findmnt -n -o FSTYPE / | grep -qE "^(ext4|overlay)$" && "$P" -V'
t procps-ng ps '"$P" -eo pid,comm | grep -q "^ *1 init" && /bin/free -m | grep -q "^Mem:" && /bin/vmstat 1 2 >/dev/null && /bin/top -bn1 | grep -q "^top - " && "$P" -V'
echo "buildroot-test: $pass/$n passed"

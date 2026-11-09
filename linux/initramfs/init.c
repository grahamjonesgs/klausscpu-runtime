// SPDX-License-Identifier: GPL-2.0
/*
 * A libc-free /init for LINUX_PLAN.md milestone 3: user mode, SYSCALL,
 * TLB refill and page faults, ELF exec, fork/wait.  It writes through
 * /dev/kmsg (there is no tty console yet), so its lines appear on the
 * boot console.
 *
 * Syscall ABI: number in r12, arguments in r0-r5, result in r12.
 */
#define __NR_openat	56
#define __NR_write	64
#define __NR_exit	93
#define __NR_exit_group	94
#define __NR_sched_yield 124
#define __NR_getpid	172
#define __NR_brk	214
#define __NR_clone	220
#define __NR_wait4	260

#define AT_FDCWD	-100
#define O_WRONLY	1
#define SIGCHLD		17

static long sys3(long n, long a, long b, long c)
{
	register long r12 asm("r12") = n;
	register long r0 asm("r0") = a;
	register long r1 asm("r1") = b;
	register long r2 asm("r2") = c;

	asm volatile("syscall" : "+r"(r12) : "r"(r0), "r"(r1), "r"(r2) : "memory");
	return r12;
}

static long sys5(long n, long a, long b, long c, long d, long e)
{
	register long r12 asm("r12") = n;
	register long r0 asm("r0") = a;
	register long r1 asm("r1") = b;
	register long r2 asm("r2") = c;
	register long r3 asm("r3") = d;
	register long r4 asm("r4") = e;

	asm volatile("syscall" : "+r"(r12) : "r"(r0), "r"(r1), "r"(r2), "r"(r3), "r"(r4)
		     : "memory");
	return r12;
}

static int out = -1;

static unsigned long slen(const char *s)
{
	unsigned long n = 0;

	while (s[n])
		n++;
	return n;
}

static void say(const char *s)
{
	sys3(__NR_write, out, (long)s, slen(s));
}

static char *num(char *p, long v)
{
	char tmp[24];
	int n = 0;

	if (v < 0) {
		*p++ = '-';
		v = -v;
	}
	do {
		tmp[n++] = '0' + v % 10;
		v /= 10;
	} while (v);
	while (n)
		*p++ = tmp[--n];
	return p;
}

static void say_num(const char *what, long v)
{
	char buf[96], *p = buf;
	const char *s = what;

	while (*s)
		*p++ = *s++;
	p = num(p, v);
	*p++ = '\n';
	*p = 0;
	say(buf);
}

static volatile char bss_area[200000];		/* several pages of BSS: demand-zero faults */
static volatile long data_word = 1234;		/* .data: a file-backed private page */

void __attribute__((noreturn)) _start(void)
{
	long pid, child, status = 0, sum = 0, i;

	out = sys3(__NR_openat, AT_FDCWD, (long)"/dev/kmsg", O_WRONLY);
	say("init: hello from user space\n");
	pid = sys3(__NR_getpid, 0, 0, 0);
	say_num("init: getpid = ", pid);

	for (i = 0; i < (long)sizeof(bss_area); i += 4096)
		bss_area[i] = (char)i;
	for (i = 0; i < (long)sizeof(bss_area); i += 4096)
		sum += bss_area[i];
	say_num("init: bss touched, checksum ", sum);
	data_word += 1;
	say_num("init: data_word = ", data_word);

	child = sys5(__NR_clone, SIGCHLD, 0, 0, 0, 0);
	if (child == 0) {
		data_word = 99;
		say_num("init: child pid ", sys3(__NR_getpid, 0, 0, 0));
		say_num("init: child sees data_word = ", data_word);
		sys3(__NR_exit, 7, 0, 0);
	}
	say_num("init: forked child ", child);
	sys5(__NR_wait4, child, (long)&status, 0, 0, 0);
	say_num("init: child exit status ", (status >> 8) & 0xff);
	say_num("init: parent still sees data_word = ", data_word);
	say("init: milestone 3 done, idling\n");
	for (;;)
		sys3(__NR_sched_yield, 0, 0, 0);
}

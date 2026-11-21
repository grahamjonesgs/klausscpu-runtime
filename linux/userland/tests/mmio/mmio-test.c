// SPDX-License-Identifier: GPL-2.0
/*
 * mmio-test: /dev/klausscpu-mmio (arch/klausscpu/kernel/klausscpu_mmio.c).
 * Run as root.  Prints "PASS name", "FAIL name: why" or "SKIP name: why"
 * per check and ends with "mmio-test: PASS" or "mmio-test: FAIL (n)".
 * The checks of KLMMIO_WAIT expect the switches to stay put while they run
 * (about a second).
 *
 * mmio-test --wait-switch SECONDS: wait up to SECONDS (1..3600) for a
 * switch to be flipped and print the old and new values; exits 0 if one
 * was flipped, 1 if not.
 *
 * On the board the LED bar shows 0x5A5A briefly and is then restored.  The
 * klausscc emulator models the LEDs too (klausscc board-io), so the
 * read-back must be 0x5A5A on both.
 */
#include <errno.h>
#include <grp.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <linux/capability.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <asm/klausscpu-mmio.h>

#define DEV "/dev/klausscpu-mmio"

static int fails;

static void pass(const char *n) { printf("PASS %s\n", n); }
static void fail(const char *n, const char *why)
{
	printf("FAIL %s: %s\n", n, why);
	fails++;
}

static int rd(int fd, uint64_t addr, uint64_t *v)
{
	struct klmmio_access a = { .addr = addr, .value = 0 };

	if (ioctl(fd, KLMMIO_READ, &a) < 0)
		return -errno;
	*v = a.value;
	return 0;
}

static int wr(int fd, uint64_t addr, uint64_t v)
{
	struct klmmio_access a = { .addr = addr, .value = v };

	return ioctl(fd, KLMMIO_WRITE, &a) < 0 ? -errno : 0;
}

/* KLMMIO_WAIT; *v gets the value the driver wrote back. */
static int wt(int fd, uint64_t addr, uint64_t mask, uint64_t old, uint32_t ms, uint32_t flags,
	      uint64_t *v)
{
	struct klmmio_wait w = {
		.addr = addr, .mask = mask, .old = old, .timeout_ms = ms, .flags = flags,
		.value = 0xDEADBEEFDEADBEEFULL,
	};
	int rc = ioctl(fd, KLMMIO_WAIT, &w) < 0 ? -errno : 0;

	*v = w.value;
	return rc;
}

/* An access that must fail with errno e. */
static void expect_err(const char *n, int rc, int e)
{
	char why[96];

	if (rc == -e) {
		pass(n);
		return;
	}
	snprintf(why, sizeof(why), "got %s, want %s", rc ? strerror(-rc) : "success", strerror(e));
	fail(n, why);
}

static int64_t mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint64_t now_ms(int fd)
{
	uint64_t v = 0;

	rd(fd, KLMMIO_CLOCK_MS, &v);
	return v;
}

static void on_alarm(int sig) { (void)sig; }

/* CPU time (user + system) used by this process so far, in ms. */
static int64_t cpu_ms(void)
{
	struct rusage ru;

	getrusage(RUSAGE_SELF, &ru);
	return ((int64_t)ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000 +
	       (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1000;
}

/*
 * One KLMMIO_WAIT on SWITCHES with old = sw, which must end with errno e
 * (0: success) after lo..hi ms by CLOCK_MONOTONIC, writing back want.
 */
static void wait_check(const char *n, int fd, uint64_t old, uint32_t ms, int e, uint64_t want,
		       int64_t lo, int64_t hi)
{
	char why[128];
	uint64_t v;
	int64_t m0 = mono_ms(), dm;
	int rc = wt(fd, KLMMIO_SWITCHES, 0xFFFF, old, ms, 0, &v);

	dm = mono_ms() - m0;
	printf("INFO %s: %s after %lld ms, value 0x%llx\n", n, rc ? strerror(-rc) : "success",
	       (long long)dm, (unsigned long long)v);
	if (rc != -e)
		snprintf(why, sizeof(why), "got %s, want %s", rc ? strerror(-rc) : "success",
			 e ? strerror(e) : "success");
	else if (v != want)
		snprintf(why, sizeof(why), "value 0x%llx, want 0x%llx", (unsigned long long)v,
			 (unsigned long long)want);
	else if (dm < lo || dm > hi)
		snprintf(why, sizeof(why), "took %lld ms, want %lld..%lld", (long long)dm,
			 (long long)lo, (long long)hi);
	else {
		pass(n);
		return;
	}
	fail(n, why);
}

/* mmio-test --wait-switch SECONDS (for a person at the board). */
static int wait_switch(const char *arg)
{
	char *end;
	long s = strtol(arg, &end, 10);
	uint64_t old, v;
	int64_t m0;
	int fd, rc;

	if (*arg == 0 || *end || s < 1 || s > 3600) {
		fprintf(stderr, "mmio-test: --wait-switch takes 1..3600 seconds\n");
		return 2;
	}
	fd = open(DEV, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "mmio-test: " DEV ": %s\n", strerror(errno));
		return 2;
	}
	if ((rc = rd(fd, KLMMIO_SWITCHES, &old))) {
		fprintf(stderr, "mmio-test: reading SWITCHES: %s\n", strerror(-rc));
		close(fd);
		return 2;
	}
	printf("switches 0x%04llx: flip one within %ld s\n", (unsigned long long)old, s);
	fflush(stdout);
	m0 = mono_ms();
	/* EINTR (e.g. Ctrl-Z and fg): wait again for the time left. */
	do {
		int64_t left = s * 1000 - (mono_ms() - m0);

		rc = wt(fd, KLMMIO_SWITCHES, 0xFFFF, old, left > 0 ? (uint32_t)left : 0, 0, &v);
	} while (rc == -EINTR);
	close(fd);
	if (rc == 0) {
		printf("switches changed after %lld ms: old 0x%04llx new 0x%04llx\n",
		       (long long)(mono_ms() - m0), (unsigned long long)old, (unsigned long long)v);
		return 0;
	}
	printf("no change after %lld ms (%s): switches 0x%04llx\n", (long long)(mono_ms() - m0),
	       strerror(-rc), (unsigned long long)v);
	return 1;
}

int main(int argc, char **argv)
{
	char why[128];
	uint64_t v, feat = 0, leds, back, t0, t1, sw = 0;
	int64_t m0, m1, dc, dm;
	struct stat st;
	int fd, rc;

	if (argc == 3 && !strcmp(argv[1], "--wait-switch"))
		return wait_switch(argv[2]);
	if (argc != 1) {
		fprintf(stderr, "usage: mmio-test [--wait-switch SECONDS]\n");
		return 2;
	}
	if (stat(DEV, &st) < 0) {
		printf("FAIL node: " DEV ": %s\nmmio-test: FAIL (1)\n", strerror(errno));
		return 1;
	}
	if (S_ISCHR(st.st_mode) && (st.st_mode & 07777) == 0600 && st.st_uid == 0 && st.st_gid == 0)
		pass("node");
	else {
		snprintf(why, sizeof(why), "mode %o uid %u gid %u, want a char device 0600 root:root",
			 (unsigned)st.st_mode, (unsigned)st.st_uid, (unsigned)st.st_gid);
		fail("node", why);
	}
	fd = open(DEV, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		printf("FAIL open: %s\nmmio-test: FAIL (%d)\n", strerror(errno), fails + 1);
		return 1;
	}

	/* CPU_FEATURES. */
	rc = rd(fd, KLMMIO_CPU_FEATURES, &feat);
	if (rc)
		fail("cpu-features", strerror(-rc));
	else if (!(feat & 1))
		fail("cpu-features", "bit 0 (exceptions, which Linux needs) is clear");
	else {
		printf("INFO CPU_FEATURES 0x%llx\n", (unsigned long long)feat);
		pass("cpu-features");
	}

	/* LEDS: write 0x5A5A, read it back, restore. */
	rc = rd(fd, KLMMIO_LEDS, &leds);
	if (rc)
		fail("leds-read", strerror(-rc));
	else if ((rc = wr(fd, KLMMIO_LEDS, 0x5A5A)))
		fail("leds-write", strerror(-rc));
	else {
		if ((rc = rd(fd, KLMMIO_LEDS, &back)))
			fail("leds-readback", strerror(-rc));
		else if (back == 0x5A5A)
			pass("leds-readback");
		else {
			snprintf(why, sizeof(why), "read 0x%llx, want 0x5a5a", (unsigned long long)back);
			fail("leds-readback", why);
		}
		rc = wr(fd, KLMMIO_LEDS, leds);
		if (rc)
			fail("leds-restore", strerror(-rc));
		else if ((rc = rd(fd, KLMMIO_LEDS, &back)) || back != leds)
			fail("leds-restore", rc ? strerror(-rc) : "read-back differs");
		else
			pass("leds-restore");
	}

	/* SWITCHES: 16 bits. */
	rc = rd(fd, KLMMIO_SWITCHES, &v);
	if (rc)
		fail("switches", strerror(-rc));
	else if (v > 0xFFFF) {
		snprintf(why, sizeof(why), "0x%llx is wider than 16 bits", (unsigned long long)v);
		fail("switches", why);
	} else {
		printf("INFO SWITCHES 0x%04llx\n", (unsigned long long)v);
		pass("switches");
	}
	sw = rc ? 0 : v;

	/* The other readable registers. */
	{
		static const struct { const char *n; uint64_t a; } r[] = {
			{ "read-rgb1", KLMMIO_RGB1 }, { "read-rgb2", KLMMIO_RGB2 },
			{ "read-seg-low", KLMMIO_SEG_LOW }, { "read-seg-high", KLMMIO_SEG_HIGH },
			{ "read-seg-all", KLMMIO_SEG_ALL }, { "read-timer-count", KLMMIO_TIMER_COUNT },
		};
		for (unsigned i = 0; i < sizeof(r) / sizeof(r[0]); i++) {
			rc = rd(fd, r[i].a, &v);
			if (rc)
				fail(r[i].n, strerror(-rc));
			else
				pass(r[i].n);
		}
	}

	/* CLOCK_MS advances by about 200 ms over usleep(200000). */
	/*
	 * On a loaded board the sleep may last much longer, so the check is
	 * against the kernel's monotonic clock over the same interval.
	 */
	m0 = mono_ms();
	t0 = now_ms(fd);
	usleep(200000);
	t1 = now_ms(fd);
	m1 = mono_ms();
	dc = (int64_t)(t1 - t0);
	dm = m1 - m0;
	printf("INFO CLOCK_MS %llu -> %llu (+%lld ms; CLOCK_MONOTONIC +%lld ms)\n",
	       (unsigned long long)t0, (unsigned long long)t1, (long long)dc, (long long)dm);
	if (dc >= 190 && dc <= dm + 50 && dc >= dm - 50)
		pass("clock-ms");
	else
		fail("clock-ms", "not >= 190 ms and within 50 ms of CLOCK_MONOTONIC");

	/* Refused accesses. */
	expect_err("write-switches", wr(fd, KLMMIO_SWITCHES, 0), EPERM);
	expect_err("write-clock-ms", wr(fd, KLMMIO_CLOCK_MS, 0), EPERM);
	expect_err("write-cpu-features", wr(fd, KLMMIO_CPU_FEATURES, 0), EPERM);
	expect_err("read-seg-blank", rd(fd, KLMMIO_SEG_BLANK, &v), EPERM);
	/*
	 * The refused probes are chosen to be harmless should a broken driver
	 * let them through: non-destructive reads, and writes to read-only or
	 * reserved registers (not UART RX, which pops, nor the timer period
	 * or vectors, which would stop or flood the tick).
	 */
	expect_err("read-uart-status", rd(fd, 0xF0010010ULL, &v), EPERM);
	expect_err("read-int-mask", rd(fd, 0xF00F0000ULL, &v), EPERM);
	expect_err("write-int-pending", wr(fd, 0xF00F0008ULL, 0), EPERM);
	expect_err("write-intc-reserved", wr(fd, 0xF00F0060ULL, 0), EPERM);
	expect_err("read-trng-data", rd(fd, 0xF00C0010ULL, &v), EPERM);
	expect_err("read-sd-ctrl", rd(fd, 0xF0000000ULL, &v), EPERM);
	expect_err("write-cache-ctrl", wr(fd, 0xF0050000ULL, 2), EPERM);
	expect_err("read-unlisted-offset", rd(fd, 0xF0040010ULL, &v), EPERM);
	expect_err("read-ram", rd(fd, 0x1000ULL, &v), EPERM);
	expect_err("read-high-alias", rd(fd, 0x1F0040000ULL, &v), EPERM);
	expect_err("read-top", rd(fd, 0xFFFFFFFFFFFFFFF8ULL, &v), EPERM);
	expect_err("read-misaligned", rd(fd, 0xF0040004ULL, &v), EINVAL);
	expect_err("write-misaligned", wr(fd, 0xF0040001ULL, 0), EINVAL);
	expect_err("read-misaligned-uart", rd(fd, 0xF0010009ULL, &v), EINVAL);
	expect_err("bad-pointer", ioctl(fd, KLMMIO_READ, (void *)8) < 0 ? -errno : 0, EFAULT);
	{
		struct klmmio_access a = { .addr = KLMMIO_LEDS, .value = 0 };

		expect_err("bad-ioctl", ioctl(fd, _IOWR(KLMMIO_IOC_MAGIC, 0x7f, struct klmmio_access), &a) < 0
			   ? -errno : 0, ENOTTY);
	}

	/*
	 * KLMMIO_WAIT on SWITCHES (sw, read above).  A different old returns
	 * at once; the same old times out, after about the timeout.
	 */
	wait_check("wait-changed", fd, ~sw, 1000, 0, sw, 0, 100);
	wait_check("wait-zero-changed", fd, ~sw, 0, 0, sw, 0, 100);
	wait_check("wait-zero", fd, sw, 0, ETIMEDOUT, sw, 0, 100);
	{
		/* Sleeping, not spinning: little CPU time over the 200 ms. */
		int64_t c = cpu_ms();

		wait_check("wait-timeout", fd, sw, 200, ETIMEDOUT, sw, 200, 260);
		c = cpu_ms() - c;
		printf("INFO wait-sleeps: %lld ms of CPU time\n", (long long)c);
		if (c < 100)
			pass("wait-sleeps");
		else
			fail("wait-sleeps", "the 200 ms wait used 100 ms or more of CPU time");
	}
	/* Two processes waiting at once: both time out after about 200 ms. */
	{
		int64_t t = mono_ms();
		int status = 0;
		pid_t pid = fork();

		if (pid == 0) {
			int64_t c0 = mono_ms(), d;

			rc = wt(fd, KLMMIO_SWITCHES, 0xFFFF, sw, 200, 0, &v);
			d = mono_ms() - c0;
			_exit(rc == -ETIMEDOUT && v == sw && d >= 200 && d <= 300 ? 0 : 1);
		}
		if (pid < 0)
			fail("wait-concurrent", strerror(errno));
		else {
			uint64_t pv;
			int prc = wt(fd, KLMMIO_SWITCHES, 0xFFFF, sw, 200, 0, &pv);

			if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status))
				fail("wait-concurrent", "the child's wait was not ETIMEDOUT after 200..300 ms");
			else if (prc != -ETIMEDOUT || pv != sw)
				fail("wait-concurrent", "the parent's wait was not ETIMEDOUT");
			else if (mono_ms() - t > 360)
				fail("wait-concurrent", "the two waits took over 360 ms together");
			else
				pass("wait-concurrent");
		}
	}
	/* The longest timeout is accepted (old differs, so no wait). */
	wait_check("wait-timeout-max-ok", fd, ~sw, KLMMIO_WAIT_MAX_MS, 0, sw, 0, 100);
	{
		/* A bit outside the mask that differs doesn't count. */
		int64_t t = mono_ms();

		rc = wt(fd, KLMMIO_SWITCHES, 0x0001, sw ^ 0xFFFE, 50, 0, &v);
		t = mono_ms() - t;
		if (rc == -ETIMEDOUT && v == sw && t >= 40)
			pass("wait-mask");
		else {
			snprintf(why, sizeof(why), "%s after %lld ms, value 0x%llx",
				 rc ? strerror(-rc) : "success", (long long)t, (unsigned long long)v);
			fail("wait-mask", why);
		}
	}
	/*
	 * A signal during a 2 s wait: EINTR soon after it, though the handler
	 * has SA_RESTART (the call is not restarted).
	 */
	{
		struct sigaction sa;
		struct itimerval it = { .it_value = { 0, 300000 } }, off = { 0 };

		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = on_alarm;
		sa.sa_flags = SA_RESTART;
		sigemptyset(&sa.sa_mask);
		if (sigaction(SIGALRM, &sa, NULL) || setitimer(ITIMER_REAL, &it, NULL))
			fail("wait-eintr", strerror(errno));
		else
			wait_check("wait-eintr", fd, sw, 2000, EINTR, sw, 250, 1000);
		setitimer(ITIMER_REAL, &off, NULL);
		signal(SIGALRM, SIG_DFL);
	}
	expect_err("wait-leds", wt(fd, KLMMIO_LEDS, 0xFFFF, 0, 0, 0, &v), EPERM);
	expect_err("wait-clock-ms", wt(fd, KLMMIO_CLOCK_MS, ~0ULL, 0, 0, 0, &v), EPERM);
	expect_err("wait-seg-blank", wt(fd, KLMMIO_SEG_BLANK, ~0ULL, 0, 0, 0, &v), EPERM);
	expect_err("wait-unlisted", wt(fd, 0xF0010010ULL, ~0ULL, 0, 0, 0, &v), EPERM);
	expect_err("wait-misaligned", wt(fd, KLMMIO_SWITCHES + 4, 0xFFFF, 0, 0, 0, &v), EINVAL);
	expect_err("wait-flags", wt(fd, KLMMIO_SWITCHES, 0xFFFF, ~sw, 0, 1, &v), EINVAL);
	expect_err("wait-flags-high", wt(fd, KLMMIO_SWITCHES, 0xFFFF, ~sw, 0, 0x80000000U, &v), EINVAL);
	expect_err("wait-timeout-max", wt(fd, KLMMIO_SWITCHES, 0xFFFF, ~sw, KLMMIO_WAIT_MAX_MS + 1, 0, &v),
		   EINVAL);
	expect_err("wait-timeout-huge", wt(fd, KLMMIO_SWITCHES, 0xFFFF, ~sw, 0xFFFFFFFFU, 0, &v), EINVAL);
	expect_err("wait-bad-pointer", ioctl(fd, KLMMIO_WAIT, (void *)8) < 0 ? -errno : 0, EFAULT);
	{
		/* A struct the driver can read but not write back: EFAULT. */
		struct klmmio_wait *w = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
					     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

		if (w == MAP_FAILED)
			fail("wait-readonly-struct", strerror(errno));
		else {
			w->addr = KLMMIO_SWITCHES;
			w->mask = 0xFFFF;
			w->old = ~sw;
			if (mprotect(w, 4096, PROT_READ))
				fail("wait-readonly-struct", strerror(errno));
			else
				expect_err("wait-readonly-struct", ioctl(fd, KLMMIO_WAIT, w) < 0 ? -errno : 0,
					   EFAULT);
			munmap(w, 4096);
		}
	}
	close(fd);

	/* Read-only open: reads work, writes are EBADF. */
	fd = open(DEV, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		fail("rdonly-open", strerror(errno));
	else {
		rc = rd(fd, KLMMIO_SWITCHES, &v);
		if (rc)
			fail("rdonly-read", strerror(-rc));
		else
			pass("rdonly-read");
		expect_err("rdonly-write", wr(fd, KLMMIO_LEDS, 0), EBADF);
		rc = wt(fd, KLMMIO_SWITCHES, 0xFFFF, ~sw, 0, 0, &v);
		if (rc)
			fail("rdonly-wait", strerror(-rc));
		else
			pass("rdonly-wait");
		close(fd);
	}

	/* Write-only open: writes work, reads are EBADF. */
	fd = open(DEV, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		fail("wronly-open", strerror(errno));
	else {
		expect_err("wronly-read", rd(fd, KLMMIO_SWITCHES, &v), EBADF);
		expect_err("wronly-wait", wt(fd, KLMMIO_SWITCHES, 0xFFFF, ~sw, 0, 0, &v), EBADF);
		close(fd);
	}

	/* Non-root: a child drops to nobody (65534) and must not open it. */
	if (getuid() != 0)
		printf("SKIP nonroot-open: not run as root\n");
	else {
		pid_t pid = fork();
		int status = 0;

		if (pid == 0) {
			if (setgroups(0, NULL) || setgid(65534) || setuid(65534))
				_exit(2);
			int f = open(DEV, O_RDONLY);
			_exit(f >= 0 ? 1 : errno == EACCES ? 0 : 3);
		}
		if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
			fail("nonroot-open", "child failed");
		else if (WEXITSTATUS(status) == 0)
			pass("nonroot-open");
		else if (WEXITSTATUS(status) == 1)
			fail("nonroot-open", "uid 65534 opened it");
		else if (WEXITSTATUS(status) == 2)
			fail("nonroot-open", "setuid failed");
		else
			fail("nonroot-open", "open failed, but not with EACCES");
	}

	/* Root without CAP_SYS_RAWIO: a child drops every capability. */
	if (getuid() != 0)
		printf("SKIP nocap-open: not run as root\n");
	else {
		pid_t pid = fork();
		int status = 0;

		if (pid == 0) {
			struct __user_cap_header_struct h = { _LINUX_CAPABILITY_VERSION_3, 0 };
			struct __user_cap_data_struct d[2];

			memset(d, 0, sizeof(d));
			if (syscall(SYS_capset, &h, d))
				_exit(2);
			int f = open(DEV, O_RDONLY);
			_exit(f >= 0 ? 1 : errno == EPERM ? 0 : 3);
		}
		if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
			fail("nocap-open", "child failed");
		else if (WEXITSTATUS(status) == 0)
			pass("nocap-open");
		else if (WEXITSTATUS(status) == 1)
			fail("nocap-open", "opened without CAP_SYS_RAWIO");
		else if (WEXITSTATUS(status) == 2)
			fail("nocap-open", "capset failed");
		else
			fail("nocap-open", "open failed, but not with EPERM");
	}

	if (fails)
		printf("mmio-test: FAIL (%d)\n", fails);
	else
		printf("mmio-test: PASS\n");
	return fails != 0;
}

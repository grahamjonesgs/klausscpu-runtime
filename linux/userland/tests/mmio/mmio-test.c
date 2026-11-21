// SPDX-License-Identifier: GPL-2.0
/*
 * mmio-test: /dev/klausscpu-mmio (arch/klausscpu/kernel/klausscpu_mmio.c).
 * Run as root.  Prints "PASS name", "FAIL name: why" or "SKIP name: why"
 * per check and ends with "mmio-test: PASS" or "mmio-test: FAIL (n)".
 *
 * On the board the LED bar shows 0x5A5A briefly and is then restored.  The
 * klausscc emulator models the LEDs too (klausscc board-io), so the
 * read-back must be 0x5A5A on both.
 */
#include <errno.h>
#include <grp.h>
#include <time.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
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

int main(void)
{
	char why[128];
	uint64_t v, feat = 0, leds, back, t0, t1;
	int64_t m0, m1, dc, dm;
	struct stat st;
	int fd, rc;

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
		close(fd);
	}

	/* Write-only open: writes work, reads are EBADF. */
	fd = open(DEV, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		fail("wronly-open", strerror(errno));
	else {
		expect_err("wronly-read", rd(fd, KLMMIO_SWITCHES, &v), EBADF);
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

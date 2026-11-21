/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_ASM_KLAUSSCPU_MMIO_H
#define _UAPI_ASM_KLAUSSCPU_MMIO_H

/*
 * /dev/klausscpu-mmio: user access to a whitelisted set of the board's
 * MMIO registers (arch/klausscpu/kernel/klausscpu_mmio.c).  User space
 * cannot map MMIO on KlaussCPU (a PTE holds RAM page frames only and has
 * no cache-inhibit bit), so the kernel does each access, as one 64-bit
 * load or store through its uncached MMIO segment.
 *
 * addr is the register's physical address (e.g. KLMMIO_LEDS), 8-byte
 * aligned; value is the whole 64-bit register (bits above the register's
 * width read 0 and are ignored on write).  Errors (errno):
 *   EPERM   open without CAP_SYS_RAWIO
 *   EINVAL  addr is not 8-byte aligned
 *   EPERM   addr is not in the whitelist, or the register does not allow
 *           this direction (e.g. a write to SWITCHES)
 *   EBADF   KLMMIO_READ or KLMMIO_WAIT on a file not opened for reading,
 *           KLMMIO_WRITE on one not opened for writing
 *   EFAULT  a bad pointer
 *   ENOTTY  an unknown ioctl
 * The device node is root-only (0600).
 *
 * KLMMIO_WAIT blocks until a register changes: it returns 0 as soon as
 * (register & mask) != (old & mask), checking at once and then sampling
 * about every 10 ms (the inputs have no interrupt).  timeout_ms 0 checks
 * once without sleeping.  Only the registers marked "wait" below (inputs,
 * which change on their own but not all the time) can be waited on; today
 * that is SWITCHES.  value holds the last sample on return, also with
 * ETIMEDOUT and EINTR.  mask 0 never sees a change, so it just sleeps for
 * timeout_ms.  Errors beyond those above:
 *   ETIMEDOUT  no change within timeout_ms
 *   EINTR      a signal arrived; the call is never restarted (SA_RESTART
 *              or not), so a periodic signal can't stretch the wait.  A
 *              stop and continue (SIGSTOP/SIGCONT), a ptrace attach or the
 *              freezer also give EINTR, with no handler run: retry with
 *              the time left
 *   EINVAL     flags != 0, or timeout_ms > KLMMIO_WAIT_MAX_MS
 *   EPERM      addr is readable but not waitable (e.g. LEDS, CLOCK_MS)
 */

#include <linux/ioctl.h>
#include <linux/types.h>

struct klmmio_access {
	__u64 addr;	/* physical register address */
	__u64 value;	/* KLMMIO_READ: out; KLMMIO_WRITE: in */
};

struct klmmio_wait {
	__u64 addr;		/* register address (waitable) */
	__u64 mask;		/* the bits that matter */
	__u64 old;		/* the caller's last value */
	__u32 timeout_ms;	/* 0: check once, no sleep; at most KLMMIO_WAIT_MAX_MS */
	__u32 flags;		/* must be 0 */
	__u64 value;		/* out: the register when the call returned */
};

#define KLMMIO_WAIT_MAX_MS	3600000U	/* one hour */

#define KLMMIO_IOC_MAGIC	0xC1
/*
 * With the generic ioctl encoding: KLMMIO_READ 0xC010C100, KLMMIO_WRITE
 * 0x4010C101, KLMMIO_WAIT 0xC028C103.
 */
#define KLMMIO_READ	_IOWR(KLMMIO_IOC_MAGIC, 0, struct klmmio_access)
#define KLMMIO_WRITE	_IOW(KLMMIO_IOC_MAGIC, 1, struct klmmio_access)
/* nr 2 is reserved for a batch of accesses in one call (not implemented). */
#define KLMMIO_WAIT	_IOWR(KLMMIO_IOC_MAGIC, 3, struct klmmio_wait)

/*
 * The whitelisted registers (klaussCPU MMIO_MAP.md); R, W or RW, and
 * "wait" if KLMMIO_WAIT takes it.  After a
 * SEG_BLANK write the SEG_* registers read 0x22 per byte, which no packed
 * digit write reproduces: the display's state can't be saved and restored
 * through these registers.
 */
#define KLMMIO_RGB1		0xF0020000ULL	/* RW  [11:8] R, [7:4] G, [3:0] B */
#define KLMMIO_RGB2		0xF0020008ULL	/* RW */
#define KLMMIO_SEG_LOW		0xF0030000ULL	/* W: 4 hex digits [15:0]; R: raw padded lower display */
#define KLMMIO_SEG_HIGH		0xF0030008ULL	/* W: 4 hex digits [15:0]; R: raw padded upper display */
#define KLMMIO_SEG_ALL		0xF0030010ULL	/* W: 8 hex digits [31:0]; R: {upper, lower} padded */
#define KLMMIO_SEG_BLANK	0xF0030018ULL	/* W only: any value blanks both displays */
#define KLMMIO_LEDS		0xF0040000ULL	/* RW  16-bit LED bar */
#define KLMMIO_SWITCHES		0xF0040008ULL	/* R, wait  16 slide switches */
#define KLMMIO_TIMER_COUNT	0xF00F0038ULL	/* R   timer cycle counter (32 bits) */
#define KLMMIO_CLOCK_MS		0xF00F0040ULL	/* R   ms since FPGA configuration */
#define KLMMIO_CPU_FEATURES	0xF00F0048ULL	/* R   build features */

#endif /* _UAPI_ASM_KLAUSSCPU_MMIO_H */

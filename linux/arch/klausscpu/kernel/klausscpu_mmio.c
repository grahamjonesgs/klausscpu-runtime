// SPDX-License-Identifier: GPL-2.0
/*
 * /dev/klausscpu-mmio: user access to a whitelist of board registers
 * (uapi asm/klausscpu-mmio.h), for the Kl runtime (kl KL_LANGUAGE_PLAN.md
 * §5) and board tools.
 *
 * User space cannot map MMIO on KlaussCPU: a PTE holds RAM page frames only
 * (_PAGE_PFN_MASK) and has no cache-inhibit bit, and /dev/mem reaches RAM
 * only (mm/init.c).  So the kernel does the access: one readq or writeq of
 * the register through the uncached MMIO segment (MMIO_BASE, where virtual
 * = physical, as in every other driver here).
 *
 * Only the registers in klmmio_regs[] can be reached, each in the
 * directions it lists; everything else is EPERM, and an address that is not
 * 8-byte aligned is EINVAL.  The address must equal a table entry in all
 * 64 bits (the bus decodes only EA[31:0], so a looser match could alias),
 * so only whitelisted addresses reach the bus.  Each access is one 64-bit
 * load or store,
 * with no read-modify-write, so no lock is needed: concurrent writers get
 * last-writer-wins, as on bare metal.  No whitelisted register has a read
 * side effect, and none is used by the kernel.
 *
 * The node is root-only (0600; /etc/mdev.conf keeps that), and open needs
 * CAP_SYS_RAWIO, as /dev/mem does.  KLMMIO_READ and KLMMIO_WAIT need a file
 * opened for reading and KLMMIO_WRITE one opened for writing.
 *
 * KLMMIO_WAIT blocks until a register marked waitable changes in the bits
 * the caller names.  The board's inputs have no interrupt, so it samples:
 * one readq about every KLMMIO_WAIT_PERIOD_MS, sleeping on an hrtimer in
 * between (interruptible, no busy-wait).  Each waiter has only its own
 * stack state, so any number of processes may wait at once.
 *
 * Not reachable, on purpose: the UART (the console), SD, Ethernet, cache
 * control, QSPI, the interrupt controller and timer period, the TRNG (the
 * kernel's hw_random), the crypto blocks, the blitter and VGA (both DMA to
 * any physical address), the performance counters and the core-2 mailbox.
 */
#include <linux/capability.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/hrtimer.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/miscdevice.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <asm/klausscpu.h>
#include <asm/klausscpu-mmio.h>

#define KLMMIO_R	0x1
#define KLMMIO_W	0x2
#define KLMMIO_RW	(KLMMIO_R | KLMMIO_W)
/* KLMMIO_WAIT may block on it (only with KLMMIO_R). */
#define KLMMIO_WAITABLE	0x4
#define KLMMIO_ALLOW	(KLMMIO_RW | KLMMIO_WAITABLE)

/*
 * KLMMIO_WAIT's sampling period (the longest timeout is in the uapi).  The
 * clockevent is periodic only (time.c), so hrtimers run in low-res mode
 * and expire on the HZ tick: a sample comes every 12 ms at HZ=250, and the
 * slack only counts should a oneshot clockevent appear.
 */
#define KLMMIO_WAIT_PERIOD_MS	10
#define KLMMIO_WAIT_SLACK_NS	(1 * NSEC_PER_MSEC)

struct klmmio_reg {
	u64 addr;
	unsigned int allow;	/* KLMMIO_R, KLMMIO_W, KLMMIO_WAITABLE */
};

/*
 * The whitelist.  To add a register: its physical address (in the MMIO
 * window, 8-byte aligned), the directions, and why it is safe for root's
 * programs to touch it while Linux runs: no DMA, no effect on a device the
 * kernel drives, no read side effect (a read is allowed only if reading
 * changes nothing).
 *
 * KLMMIO_WAITABLE goes only on a register that changes on its own but not
 * all the time: an input.  LEDS, RGB and SEG change only when a program
 * writes them, so a wait on them would just be a sleep; CLOCK_MS and
 * TIMER_COUNT change at every sample, so a wait on them would be a read.
 */
static const struct klmmio_reg klmmio_regs[] = {
	/* RGB LEDs: 12-bit PWM levels, cosmetic, unused by the kernel. */
	{ KLMMIO_RGB1,		KLMMIO_RW },
	{ KLMMIO_RGB2,		KLMMIO_RW },
	/*
	 * 7-segment display: cosmetic, unused by the kernel.  SEG_LOW,
	 * SEG_HIGH and SEG_ALL take packed hex digits on write and read back
	 * the raw padded digits; SEG_BLANK blanks on any write and reads
	 * nothing useful, so it is write-only.
	 */
	{ KLMMIO_SEG_LOW,	KLMMIO_RW },
	{ KLMMIO_SEG_HIGH,	KLMMIO_RW },
	{ KLMMIO_SEG_ALL,	KLMMIO_RW },
	{ KLMMIO_SEG_BLANK,	KLMMIO_W },
	/* LED bar: cosmetic, unused by the kernel; reads the last value. */
	{ KLMMIO_LEDS,		KLMMIO_RW },
	/*
	 * Slide switches: live input, read-only (the hardware drops writes).
	 * Waitable: they change when someone flips one, and only then.
	 */
	{ KLMMIO_SWITCHES,	KLMMIO_R | KLMMIO_WAITABLE },
	/*
	 * Timer block, read-only: free-running counters and the build's
	 * feature word.  Writing anything here (the vectors, INT_MASK,
	 * TIMER_PERIOD, INT2_EN) would take interrupts from the kernel.
	 */
	{ KLMMIO_TIMER_COUNT,	KLMMIO_R },
	{ KLMMIO_CLOCK_MS,	KLMMIO_R },
	{ KLMMIO_CPU_FEATURES,	KLMMIO_R },
};

/*
 * Every table entry is an aligned MMIO address, allows R or W or both and
 * no unknown bit, and is waitable only if readable (checked at boot).
 */
static bool __init klmmio_table_ok(void)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(klmmio_regs); i++) {
		u64 a = klmmio_regs[i].addr;
		unsigned int allow = klmmio_regs[i].allow;

		if (a < MMIO_BASE || a > 0xFFFFFFF8ULL || (a & 7) ||
		    !(allow & KLMMIO_RW) || (allow & ~KLMMIO_ALLOW) ||
		    ((allow & KLMMIO_WAITABLE) && !(allow & KLMMIO_R)))
			return false;
	}
	return true;
}

/* dir: the bits the entry must have, e.g. KLMMIO_R, or KLMMIO_R | KLMMIO_WAITABLE. */
static void __iomem *klmmio_lookup(u64 addr, unsigned int dir, int *err)
{
	unsigned int i;

	if (addr & 7) {
		*err = -EINVAL;
		return NULL;
	}
	for (i = 0; i < ARRAY_SIZE(klmmio_regs); i++) {
		if (klmmio_regs[i].addr != addr)
			continue;
		if ((klmmio_regs[i].allow & dir) != dir)
			break;
		return (void __iomem *)(unsigned long)klmmio_regs[i].addr;
	}
	*err = -EPERM;
	return NULL;
}

/*
 * KLMMIO_WAIT: sample the register until (value & mask) differs from
 * (old & mask) (0), timeout_ms passes (-ETIMEDOUT) or a signal comes
 * (-EINTR).  Every return after the first read stores the last sample in
 * up->value.  -EINTR, not -ERESTARTSYS: a restarted call would start its
 * timeout again, so a periodic signal could stretch the wait for ever.
 * Anything that sets the signal-pending flag ends it so, also with no
 * handler run: a stop and continue, a ptrace attach, the freezer.
 */
static long klmmio_wait(struct klmmio_wait __user *up)
{
	struct klmmio_wait w;
	void __iomem *reg;
	ktime_t end, now, next;
	u64 v;
	long ret;
	int err;

	if (copy_from_user(&w, up, sizeof(w)))
		return -EFAULT;
	if (w.flags || w.timeout_ms > KLMMIO_WAIT_MAX_MS)
		return -EINVAL;
	reg = klmmio_lookup(w.addr, KLMMIO_R | KLMMIO_WAITABLE, &err);
	if (!reg)
		return err;

	end = ktime_add_ms(ktime_get(), w.timeout_ms);
	for (;;) {
		v = readq(reg);
		if ((v ^ w.old) & w.mask) {
			ret = 0;
			break;
		}
		now = ktime_get();
		if (ktime_compare(now, end) >= 0) {
			ret = -ETIMEDOUT;
			break;
		}
		if (signal_pending(current)) {
			ret = -EINTR;
			break;
		}
		next = ktime_add_ms(now, KLMMIO_WAIT_PERIOD_MS);
		if (ktime_after(next, end))
			next = end;
		/*
		 * Interruptible: a signal that arrives before schedule() keeps
		 * the task runnable, so it is seen on the next pass.
		 */
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_hrtimeout_range(&next, KLMMIO_WAIT_SLACK_NS, HRTIMER_MODE_ABS);
	}
	/* copy_to_user, not put_user: up may be misaligned. */
	if (copy_to_user(&up->value, &v, sizeof(v)))
		return -EFAULT;
	return ret;
}

static long klmmio_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct klmmio_access __user *up = (struct klmmio_access __user *)arg;
	struct klmmio_access a;
	void __iomem *reg;
	int err;

	switch (cmd) {
	case KLMMIO_READ:
		if (!(file->f_mode & FMODE_READ))
			return -EBADF;
		if (copy_from_user(&a, up, sizeof(a)))
			return -EFAULT;
		reg = klmmio_lookup(a.addr, KLMMIO_R, &err);
		if (!reg)
			return err;
		a.value = readq(reg);
		if (copy_to_user(up, &a, sizeof(a)))
			return -EFAULT;
		return 0;
	case KLMMIO_WRITE:
		if (!(file->f_mode & FMODE_WRITE))
			return -EBADF;
		if (copy_from_user(&a, up, sizeof(a)))
			return -EFAULT;
		reg = klmmio_lookup(a.addr, KLMMIO_W, &err);
		if (!reg)
			return err;
		writeq(a.value, reg);
		return 0;
	case KLMMIO_WAIT:
		if (!(file->f_mode & FMODE_READ))
			return -EBADF;
		return klmmio_wait((struct klmmio_wait __user *)arg);
	default:
		return -ENOTTY;
	}
}

static int klmmio_open(struct inode *inode, struct file *file)
{
	return capable(CAP_SYS_RAWIO) ? 0 : -EPERM;
}

static const struct file_operations klmmio_fops = {
	.owner		= THIS_MODULE,
	.open		= klmmio_open,
	.unlocked_ioctl	= klmmio_ioctl,
	.llseek		= noop_llseek,
};

static struct miscdevice klmmio_dev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "klausscpu-mmio",
	.fops	= &klmmio_fops,
	.mode	= 0600,
};

static int __init klmmio_init(void)
{
	if (!klmmio_table_ok()) {
		pr_err("klausscpu-mmio: bad whitelist entry, not registered\n");
		return -EINVAL;
	}
	return misc_register(&klmmio_dev);
}
device_initcall(klmmio_init);

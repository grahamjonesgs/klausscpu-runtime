// SPDX-License-Identifier: GPL-2.0
/*
 * Misaligned user loads and stores (LINUX_PLAN "Misaligned accesses").
 *
 * A 16-bit load/store at an odd address, a 32-bit one not 4-aligned (except
 * the unaligned-tolerant MEMGET32) and, with STATUS.MAL64 set, a 64-bit one
 * not 8-aligned raise MISALIGNED_LOAD / MISALIGNED_STORE before doing
 * anything (klaussCPU CPU_ARCHITECTURE §8.3, MMU_PLAN §4.1, HW_PERF_PLAN §4):
 * EADDR is the effective address and the frame PC the instruction.  For a
 * user-mode fault we read the instruction, decode it (every class 6/7 form:
 * SIZE [25:24], SGN [23], MODE [22:21], A [20]; ISA_ENCODING_V2_MAP §2, §7.1),
 * do the access a byte at a time through get_user/put_user, write the
 * destination register and step past the instruction.
 *
 * Never emulated, so they stay SIGBUS: LR/SC (atomics), anything that isn't
 * a class 6/7 load/store, an EA that doesn't match EADDR, and every access
 * of a task that asked for prctl(PR_SET_UNALIGN, PR_UNALIGN_SIGBUS).  Stack
 * operations (PUSH/POP/CALL/RET/ENTER/LEAVE) never raise MISALIGNED.  A
 * fault on the user data during the emulated access is SIGSEGV, as the
 * access itself would have been; a store that straddles into an unwritable
 * page may have written its first bytes by then.
 */
#include <linux/entry-common.h>
#include <linux/mm.h>
#include <linux/perf_event.h>
#include <linux/prctl.h>
#include <linux/ratelimit.h>
#include <linux/sched/signal.h>
#include <linux/uaccess.h>
#include <asm/klausscpu.h>
#include <asm/setup.h>

/* Emulated accesses since boot; /proc/klausscpu_perf shows it (perf.c). */
static atomic_long_t misaligned_emulated = ATOMIC_LONG_INIT(0);

unsigned long klausscpu_misaligned_count(void)
{
	return atomic_long_read(&misaligned_emulated);
}

/* Set by klausscpu_misaligned_init when STATUS.MAL64 is on. */
static bool mal64_on;

int klausscpu_get_unalign_ctl(struct task_struct *tsk, unsigned long addr)
{
	return put_user((unsigned int)tsk->thread.align_ctl, (unsigned int __user *)addr);
}

int klausscpu_set_unalign_ctl(struct task_struct *tsk, unsigned int val)
{
	if (val & ~(PR_UNALIGN_NOPRINT | PR_UNALIGN_SIGBUS))
		return -EINVAL;
	tsk->thread.align_ctl = val;
	return 0;
}

/*
 * HW_PERF_PLAN §4: with CPU_FEATURES[23], set STATUS.MAL64 so misaligned
 * 64-bit accesses trap (in both modes) instead of silently using the
 * aligned-down doubleword; user ones are then emulated here.  Exception
 * entry and IRET leave STATUS[10] alone and only head.S writes STATUS
 * before this, so one MTSR at boot holds.
 */
void __init klausscpu_misaligned_init(void)
{
	if (!(klausscpu_cpu_features & CPU_FEAT_MAL64))
		return;
	mtsr(SR_STATUS, mfsr(SR_STATUS) | STATUS_MAL64);
	mal64_on = true;
	pr_info("KlaussCPU: misaligned 64-bit accesses trap (STATUS.MAL64)\n");
}

struct ma_insn {
	unsigned int len;	/* words */
	unsigned int bytes;	/* access size */
	bool store, sgn;
	unsigned int rd;	/* load destination / store data */
	u32 ea;
};

/*
 * Decode the class 6/7 instruction at the frame PC.  0, or -EINVAL for
 * anything we won't emulate (the caller sends SIGBUS).
 */
static int ma_decode(struct pt_regs *regs, struct ma_insn *in)
{
	u32 __user *pc = (u32 __user *)instruction_pointer(regs);
	u32 w0, w1 = 0, base, size, mode;

	if (get_user(w0, pc))
		return -EINVAL;
	in->len = w0 >> 30;
	switch ((w0 >> 26) & 0xf) {
	case 6:
		in->store = false;
		break;
	case 7:
		in->store = true;
		break;
	default:
		return -EINVAL;
	}
	size = (w0 >> 24) & 3;
	mode = (w0 >> 21) & 3;
	in->bytes = 1 << size;
	in->sgn = !in->store && (w0 & BIT(23));
	in->rd = (w0 >> 8) & 0xf;
	/*
	 * LR/SC (one word, MODE 00, [12] set, ISA_ENCODING_V2_MAP §9.2) and
	 * DCBZ ([13] set, HW_PERF_PLAN.md §1) are not plain loads or stores.
	 */
	if (in->len == 1 && mode == 0 && (w0 & (BIT(12) | BIT(13))))
		return -EINVAL;
	/*
	 * What the CPU decodes as ILLEGAL (klausscc exec.rs mem_legal): the
	 * word may not be the one that ran (rewritten since), so check it.
	 * [19:12] only in the short form; no SGN on a store; A only on a
	 * 64-bit access; MODE 10 only as two words, MODE 00/11 only as one;
	 * rs2 zero in MODE 00/01, [7:0] zero in MODE 10.
	 */
	if (((w0 >> 12) & 0xff) && !(in->len == 1 && mode == 1))
		return -EINVAL;
	if ((in->store && (w0 & BIT(23))) || ((w0 & BIT(20)) && size != 3))
		return -EINVAL;
	if (in->len == 1 ? mode == 2 : !(mode == 1 || mode == 2))
		return -EINVAL;
	if ((mode <= 1 && (w0 & 0xf)) || (mode == 2 && (w0 & 0xff)))
		return -EINVAL;
	/* Bytes never trap; 64-bit only with MAL64. */
	if (in->bytes == 1 || (in->bytes == 8 && !mal64_on))
		return -EINVAL;
	if (in->len == 2 && get_user(w1, pc + 1))
		return -EINVAL;
	base = regs->r[(w0 >> 4) & 0xf];
	switch (in->len << 2 | mode) {
	case 1 << 2 | 0:	/* [rs1] */
		in->ea = base;
		break;
	case 1 << 2 | 1:	/* short form: rs1 + (simm8 [19:12] << SIZE) */
		in->ea = base + ((u32)(s32)(s8)(w0 >> 12) << size);
		break;
	case 2 << 2 | 1:	/* rs1 + imm32 */
		in->ea = base + w1;
		break;
	case 2 << 2 | 2:	/* [imm32] */
		in->ea = w1;
		break;
	case 1 << 2 | 3:	/* rs1 + rs2 */
		in->ea = base + (u32)regs->r[w0 & 0xf];
		break;
	default:
		return -EINVAL;
	}
	/* The CPU's own EA (MMU_PLAN §4.1): a mismatch means we misread it. */
	if (in->ea != (u32)regs->eaddr)
		return -EINVAL;
	return 0;
}

/* The access a byte at a time (little-endian); -EFAULT with *bad set. */
static int ma_access(struct pt_regs *regs, const struct ma_insn *in, unsigned long *bad)
{
	u8 __user *p = (u8 __user *)(unsigned long)in->ea;
	unsigned long v = 0;
	unsigned int i;
	u8 b;

	if (in->store) {
		v = regs->r[in->rd];
		for (i = 0; i < in->bytes; i++, v >>= 8)
			if (put_user((u8)v, p + i)) {
				*bad = (unsigned long)(p + i);
				return -EFAULT;
			}
		return 0;
	}
	for (i = 0; i < in->bytes; i++) {
		if (get_user(b, p + i)) {
			*bad = (unsigned long)(p + i);
			return -EFAULT;
		}
		v |= (unsigned long)b << (8 * i);
	}
	if (in->sgn && in->bytes < 8) {
		unsigned int sh = 64 - 8 * in->bytes;

		v = (unsigned long)((long)(v << sh) >> sh);
	}
	regs->r[in->rd] = v;
	return 0;
}

static void ma_segv(unsigned long addr)
{
	struct mm_struct *mm = current->mm;
	int code = SEGV_MAPERR;

	if (mm) {
		mmap_read_lock(mm);
		if (vma_lookup(mm, addr))
			code = SEGV_ACCERR;
		mmap_read_unlock(mm);
	}
	force_sig_fault(SIGSEGV, code, (void __user *)addr);
}

/*
 * From klausscpu_do_exception for a user-mode MISALIGNED_LOAD/STORE.
 * Returns false (nothing done) when the caller should send SIGBUS: only
 * for prctl(PR_SET_UNALIGN, PR_UNALIGN_SIGBUS); an access it can't emulate
 * gets its SIGBUS here.
 */
bool klausscpu_misaligned_user(struct pt_regs *regs, unsigned long cause)
{
	unsigned long align_ctl = current->thread.align_ctl;
	struct ma_insn in;
	unsigned long bad;
	bool done = false;
	irqentry_state_t state;

	if (align_ctl & PR_UNALIGN_SIGBUS)
		return false;

	state = irqentry_enter(regs);
	local_irq_enable();
	perf_sw_event(PERF_COUNT_SW_ALIGNMENT_FAULTS, 1, regs, regs->eaddr);
	done = true;
	if (ma_decode(regs, &in) || in.store != (cause == EXC_MISALIGNED_STORE)) {
		/*
		 * Not emulated: SIGBUS now, inside this entry/exit, so that no
		 * other signal is delivered first and moves the PC it reports.
		 */
		if (show_unhandled_signals)
			pr_info_ratelimited("%s[%d]: misaligned %s at pc %08lx eaddr %08lx\n",
					    current->comm, task_pid_nr(current),
					    cause == EXC_MISALIGNED_STORE ? "store" : "load",
					    instruction_pointer(regs), regs->eaddr);
		force_sig_fault(SIGBUS, BUS_ADRALN, (void __user *)instruction_pointer(regs));
		goto out;
	}
	if (ma_access(regs, &in, &bad)) {
		if (show_unhandled_signals)
			pr_info_ratelimited("%s[%d]: segfault at %08lx in emulated misaligned %s at pc %08lx\n",
					    current->comm, task_pid_nr(current), bad,
					    in.store ? "store" : "load", instruction_pointer(regs));
		ma_segv(bad);
		goto out;
	}
	atomic_long_inc(&misaligned_emulated);
	if (!(align_ctl & PR_UNALIGN_NOPRINT))
		pr_info_ratelimited("%s[%d]: emulated misaligned %u-bit %s at pc %08lx eaddr %08x\n",
				    current->comm, task_pid_nr(current), 8 * in.bytes,
				    in.store ? "store" : "load", instruction_pointer(regs), in.ea);
	instruction_pointer_set(regs, instruction_pointer(regs) + 4 * in.len);
out:
	local_irq_disable();
	irqentry_exit(regs, state);
	return done;
}

// SPDX-License-Identifier: GPL-2.0
/*
 * Page faults: TLB misses the refill handler could not satisfy (no PTE)
 * and ACCESS faults (a permission, or a store to a clean page: hardware W
 * is clear until the PTE is dirty).
 */
#include <linux/entry-common.h>
#include <linux/extable.h>
#include <linux/mm.h>
#include <linux/perf_event.h>
#include <linux/sched/debug.h>
#include <linux/sched/signal.h>
#include <linux/uaccess.h>
#include <asm/klausscpu.h>
#include <asm/setup.h>

extern void show_regs(struct pt_regs *regs);

static void no_context(struct pt_regs *regs, unsigned long addr)
{
	if (fixup_exception(regs))
		return;
	pr_alert("Unable to handle kernel paging request at %08lx, pc %08lx\n",
		 addr, instruction_pointer(regs));
	show_regs(regs);
	panic("kernel page fault");
}

void do_page_fault(struct pt_regs *regs, unsigned long cause, unsigned long addr)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	unsigned int flags = FAULT_FLAG_DEFAULT;
	bool write = cause == EXC_TLB_MISS_STORE || cause == EXC_ACCESS_STORE;
	bool exec = cause == EXC_TLB_MISS_FETCH || cause == EXC_ACCESS_FETCH;
	irqentry_state_t state = irqentry_enter(regs);
	vm_fault_t fault;

	int code = SEGV_MAPERR;

	if (addr >= TASK_SIZE || faulthandler_disabled() || !mm) {
		if (user_mode(regs))
			goto bad_area_nosemaphore;
		no_context(regs, addr);
		goto out;
	}

	if (!regs_irqs_disabled(regs))
		local_irq_enable();
	if (user_mode(regs))
		flags |= FAULT_FLAG_USER;
	if (write)
		flags |= FAULT_FLAG_WRITE;
	if (exec)
		flags |= FAULT_FLAG_INSTRUCTION;

	perf_sw_event(PERF_COUNT_SW_PAGE_FAULTS, 1, regs, addr);
retry:
	vma = lock_mm_and_find_vma(mm, addr, regs);
	if (!vma)
		goto bad_area_nosemaphore;

	code = SEGV_ACCERR;
	if (write ? !(vma->vm_flags & VM_WRITE) :
	    exec ? !(vma->vm_flags & VM_EXEC) :
	    !(vma->vm_flags & (VM_READ | VM_WRITE | VM_EXEC)))
		goto bad_area;

	fault = handle_mm_fault(vma, addr, flags, regs);
	if (fault_signal_pending(fault, regs)) {
		if (!user_mode(regs))
			no_context(regs, addr);
		goto out;
	}
	if (fault & VM_FAULT_COMPLETED)
		goto out;
	if (fault & VM_FAULT_RETRY) {
		flags |= FAULT_FLAG_TRIED;
		goto retry;
	}
	mmap_read_unlock(mm);

	if (unlikely(fault & VM_FAULT_ERROR)) {
		if (!user_mode(regs)) {
			no_context(regs, addr);
			goto out;
		}
		if (fault & VM_FAULT_OOM) {
			pagefault_out_of_memory();
			goto out;
		}
		if (show_unhandled_signals)
			pr_info_ratelimited("%s[%d]: fault %x at %08lx pc %08lx\n",
					    current->comm, task_pid_nr(current), fault, addr,
					    instruction_pointer(regs));
		force_sig_fault((fault & VM_FAULT_SIGBUS) ? SIGBUS : SIGSEGV,
				(fault & VM_FAULT_SIGBUS) ? BUS_ADRERR : SEGV_MAPERR,
				(void __user *)addr);
	}
	goto out;

bad_area:
	mmap_read_unlock(mm);
bad_area_nosemaphore:
	if (user_mode(regs)) {
		if (show_unhandled_signals)
			pr_info_ratelimited("%s[%d]: segfault at %08lx pc %08lx cause %lu\n",
					    current->comm, task_pid_nr(current), addr,
					    instruction_pointer(regs), cause);
		force_sig_fault(SIGSEGV, code, (void __user *)addr);
		goto out;
	}
	no_context(regs, addr);
out:
	/*
	 * Generic entry wants interrupts off for irqentry_exit, as on every
	 * other way out of an exception; returning with them on let an
	 * interrupt hit RESTORE_REGS_IRET's last load (entry.S).
	 */
	local_irq_disable();
	irqentry_exit(regs, state);
}

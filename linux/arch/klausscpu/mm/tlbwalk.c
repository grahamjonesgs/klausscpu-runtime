// SPDX-License-Identifier: GPL-2.0
/*
 * The hardware TLB walker (klaussCPU MMU_PLAN §11.8, CPU_FEATURES[25]).
 *
 * On a TLB miss for a useg address with TLB_WALK.EN set, the CPU reads
 * TLB_PTB[VA[30:26]] (a pgd entry: the PA of an 8 KiB PTE table | 1) and then
 * the PTE at VA[25:16], and when that PTE is PRESENT and V writes it to the
 * TLB entry TLB_WALK.RR, exactly as the software handler (tlbex.S) would,
 * with no exception.  Anything else -- no pgd entry, a PTE not PRESENT or
 * not V (PROT_NONE), a kmap address -- raises the TLB miss as before, and
 * tlbex.S handles it.  The walker never writes a PTE: dirty and accessed
 * are emulated by the kernel already (asm/pgtable.h).
 *
 * The kernel's side: TLB_PTB is the active mm's pgd (switch_mm, and here
 * for init_mm), EN is set at boot when the CPU has the walker, and
 * /proc/klausscpu_tlbwalk reads "walker 0|1" and takes 0 or 1 to switch it
 * off and on (for measurements; "notlbwalk" on the command line keeps it
 * off from boot).  The software handler keeps its own round-robin index:
 * at worst one of them evicts an entry the other just wrote.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <asm/klausscpu.h>
#include <asm/mmu_context.h>
#include <asm/pgtable.h>
#include <asm/setup.h>

bool klausscpu_tlb_walker;	/* the CPU has it: switch_mm keeps TLB_PTB */
static bool walker_off;		/* "notlbwalk" */

static int __init notlbwalk(char *s)
{
	walker_off = true;
	return 0;
}
early_param("notlbwalk", notlbwalk);

static void walker_enable(bool on)
{
	unsigned long flags;

	local_irq_save(flags);
	/* The pgd of the mm that is active now (switch_mm keeps it after). */
	mtsr(SR_TLB_PTB, __pa(current->active_mm ? current->active_mm->pgd : swapper_pg_dir));
	mtsr(SR_TLB_WALK, on ? TLB_WALK_EN : 0);
	local_irq_restore(flags);
}

void __init klausscpu_tlbwalk_init(void)
{
	if (!(klausscpu_cpu_features & CPU_FEAT_TLBWALK))
		return;
	klausscpu_tlb_walker = true;
	mtsr(SR_TLB_WCNT, 0);
	walker_enable(!walker_off);
	pr_info("KlaussCPU: hardware TLB walker %s\n", walker_off ? "present, off (notlbwalk)" : "on");
}

/* Walker refills since boot (0 without the walker), for /proc/klausscpu_perf. */
unsigned long klausscpu_tlb_walks(void)
{
	return klausscpu_tlb_walker ? mfsr(SR_TLB_WCNT) : 0;
}

static int tlbwalk_show(struct seq_file *m, void *v)
{
	if (!klausscpu_tlb_walker)
		seq_puts(m, "walker none\n");
	else
		seq_printf(m, "walker %lu\nwalks %lu\n", mfsr(SR_TLB_WALK) & TLB_WALK_EN,
			   klausscpu_tlb_walks());
	return 0;
}

static int tlbwalk_open(struct inode *inode, struct file *file)
{
	return single_open(file, tlbwalk_show, NULL);
}

static ssize_t tlbwalk_write(struct file *file, const char __user *buf,
			     size_t len, loff_t *pos)
{
	char c;

	if (!klausscpu_tlb_walker)
		return -ENODEV;
	if (!len || get_user(c, buf))
		return -EFAULT;
	if (c != '0' && c != '1')
		return -EINVAL;
	walker_enable(c == '1');
	return len;
}

static const struct proc_ops tlbwalk_ops = {
	.proc_open	= tlbwalk_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
	.proc_write	= tlbwalk_write,
};

static int __init tlbwalk_proc_init(void)
{
	proc_create("klausscpu_tlbwalk", 0644, NULL, &tlbwalk_ops);
	return 0;
}
device_initcall(tlbwalk_proc_init);

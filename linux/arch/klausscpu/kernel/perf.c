// SPDX-License-Identifier: GPL-2.0
/*
 * /proc/klausscpu_perf: the board's performance counters (klaussCPU
 * MMIO_MAP.md "Performance counters", 0xF00D_0000, and the L1 data cache's,
 * 0xF005_0000) for tuning.  Reading prints "name value" per counter;
 * writing anything clears them all and selects exclusive stall attribution
 * (each cycle in at most one PERF_STALL_* / IF_MISS / MEM_WAIT bucket).
 * The counters are global: they count the kernel, every process and idle
 * time alike.  tlb_refills is software's: the refill handler's count
 * (arch/klausscpu/mm/tlbex.S), since the last clear; so is
 * misaligned_emulated, the user loads/stores unaligned.c emulated.  The
 * user tool is perfstat (linux/userland/perfstat.c).
 */
#include <linux/init.h>
#include <linux/io.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <asm/klausscpu.h>
#include <asm/setup.h>

#define PERF_BASE	(MMIO_BASE + 0x000D0000UL)
#define CACHE_BASE	(MMIO_BASE + 0x00050000UL)
#define PERF_CTRL_CLEAR		0x1
#define PERF_CTRL_EXCLUSIVE	0x2
#define CACHE_CTRL_CLEAR	0x1

extern unsigned long klausscpu_tlb_rr;
static unsigned long tlb_rr_base, misaligned_base;

static const struct {
	const char *name;
	unsigned long addr;
} counters[] = {
	{ "cycles",		PERF_BASE + 0x08 },
	{ "instructions",	PERF_BASE + 0x10 },
	{ "idle_cycles",	PERF_BASE + 0x40 },
	{ "interrupts",		PERF_BASE + 0x58 },
	{ "alu",		PERF_BASE + 0x60 },
	{ "loads",		PERF_BASE + 0x68 },
	{ "stores",		PERF_BASE + 0x70 },
	{ "branches",		PERF_BASE + 0x78 },
	{ "branches_taken",	PERF_BASE + 0x80 },
	{ "jumps",		PERF_BASE + 0x88 },
	{ "calls",		PERF_BASE + 0x90 },
	{ "indirect",		PERF_BASE + 0x98 },
	{ "other",		PERF_BASE + 0xA0 },
	{ "stall_data",		PERF_BASE + 0xB0 },
	{ "stall_loaduse",	PERF_BASE + 0xB8 },
	{ "stall_flags",	PERF_BASE + 0xC0 },
	{ "stall_sp",		PERF_BASE + 0xC8 },
	{ "stall_muldiv",	PERF_BASE + 0xD0 },
	{ "branch_flushes",	PERF_BASE + 0xD8 },
	{ "if_miss",		PERF_BASE + 0xE0 },
	{ "mem_wait",		PERF_BASE + 0xE8 },
	{ "ic_miss",		PERF_BASE + 0xF0 },
	{ "ic_fill_cycles",	PERF_BASE + 0xF8 },
	{ "dc_read_hits",	CACHE_BASE + 0x40 },
	{ "dc_read_misses",	CACHE_BASE + 0x48 },
	{ "dc_write_hits",	CACHE_BASE + 0x50 },
	{ "dc_write_misses",	CACHE_BASE + 0x58 },
	{ "dc_writebacks",	CACHE_BASE + 0x60 },
	{ "dc_stall_cycles",	CACHE_BASE + 0x68 },
};

static int perf_show(struct seq_file *m, void *v)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(counters); i++)
		seq_printf(m, "%s %llu\n", counters[i].name,
			   (unsigned long long)readq((void __iomem *)counters[i].addr));
	seq_printf(m, "tlb_refills %lu\n", READ_ONCE(klausscpu_tlb_rr) - tlb_rr_base);
	seq_printf(m, "misaligned_emulated %lu\n",
		   klausscpu_misaligned_count() - misaligned_base);
	return 0;
}

static int perf_open(struct inode *inode, struct file *file)
{
	return single_open(file, perf_show, NULL);
}

static ssize_t perf_write(struct file *file, const char __user *buf,
			  size_t len, loff_t *pos)
{
	writeq(PERF_CTRL_CLEAR | PERF_CTRL_EXCLUSIVE, (void __iomem *)PERF_BASE);
	writeq(CACHE_CTRL_CLEAR, (void __iomem *)CACHE_BASE);
	tlb_rr_base = READ_ONCE(klausscpu_tlb_rr);
	misaligned_base = klausscpu_misaligned_count();
	return len;
}

static const struct proc_ops perf_ops = {
	.proc_open	= perf_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
	.proc_write	= perf_write,
};

static int __init klausscpu_perf_init(void)
{
	return proc_create("klausscpu_perf", 0600, NULL, &perf_ops) ? 0 : -ENOMEM;
}
device_initcall(klausscpu_perf_init);

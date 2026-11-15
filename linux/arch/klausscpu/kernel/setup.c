// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/io.h>
#include <linux/seq_file.h>
#include <asm/sections.h>
#include <asm/setup.h>
#include <asm/klausscpu.h>
#include <asm/mmu_context.h>

unsigned long klausscpu_cpu_features;

static void __init probe_cpu(void)
{
	unsigned long f = readl((void __iomem *)(KL_INTC_BASE + KL_CPU_FEATURES));
	unsigned long need = CPU_FEAT_EXC | CPU_FEAT_TLB | CPU_FEAT_LRSC;

	klausscpu_cpu_features = f;
	if ((f & need) != need)
		panic("KlaussCPU: CPU_FEATURES %08lx lacks exceptions/TLB/LR-SC", f);
	pr_info("KlaussCPU: CPU_FEATURES %08lx, %lu TLB entries\n", f,
		CPU_FEAT_TLB_ENTRIES(f));
}

static int show_cpuinfo(struct seq_file *m, void *v)
{
	/* ICACHE_INFO (0xF005_0018): [7:0] ways, [23:8] sets, [31:24] line
	 * bytes, [63:32] total bytes; 0 on bitstreams without it. */
	u64 ic = readq((void __iomem *)(MMIO_BASE + 0x00050018UL));

	seq_printf(m, "processor\t: 0\ncpu\t\t: KlaussCPU\nfeatures\t: %08lx\n"
		   "tlb entries\t: %lu\nclock\t\t: %d Hz\n",
		   klausscpu_cpu_features, CPU_FEAT_TLB_ENTRIES(klausscpu_cpu_features),
		   CONFIG_KLAUSSCPU_CPU_HZ);
	if (ic)
		seq_printf(m, "icache\t\t: %llu KiB, %llu-way, %llu B lines\n",
			   (ic >> 32) / 1024, ic & 0xff, (ic >> 24) & 0xff);
	return 0;
}

static void *c_start(struct seq_file *m, loff_t *pos)
{
	return *pos < 1 ? (void *)1 : NULL;
}

static void *c_next(struct seq_file *m, void *v, loff_t *pos)
{
	++*pos;
	return NULL;
}

static void c_stop(struct seq_file *m, void *v)
{
}

const struct seq_operations cpuinfo_op = {
	.start	= c_start,
	.next	= c_next,
	.stop	= c_stop,
	.show	= show_cpuinfo,
};

void __init setup_arch(char **cmdline_p)
{
	klausscpu_early_console_init();
	strscpy(boot_command_line, CONFIG_CMDLINE, COMMAND_LINE_SIZE);
	*cmdline_p = boot_command_line;
	probe_cpu();

	setup_initial_init_mm(_stext, _etext, _edata, _end);

	/* All of DDR is lowmem in kseg; keep the loader's header and the image. */
	memblock_add(PHYS_OFFSET, CONFIG_KLAUSSCPU_DDR_SIZE);
	memblock_reserve(PHYS_OFFSET, __pa(_end) - PHYS_OFFSET);
	min_low_pfn = PFN_UP(PHYS_OFFSET);
	max_low_pfn = max_pfn = PFN_DOWN(PHYS_OFFSET + CONFIG_KLAUSSCPU_DDR_SIZE);
	high_memory = __va(max_low_pfn << PAGE_SHIFT);

	parse_early_param();
	paging_init();
}

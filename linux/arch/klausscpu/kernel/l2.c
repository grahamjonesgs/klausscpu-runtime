// SPDX-License-Identifier: GPL-2.0
/*
 * The unified L2 cache (klaussCPU, CPU_FEATURES[26]): 128 KiB between both
 * L1s and the DDR controller, coherent for every DDR master, so software
 * needs nothing for correctness (the L1 D-cache is flushed through
 * CACHE_CTRL as before).  This file only reports it and lets it be
 * switched off and on for measurements:
 *   /proc/klausscpu_l2  reads "l2 0|1"; takes 0 (write back and invalidate
 *                       everything, then bypass it) or 1 (use it again)
 * Its counters are in /proc/klausscpu_perf (perf.c) and its geometry in
 * /proc/cpuinfo (setup.c).  The register offsets are klaussCPU's
 * provisional ones (asm/klausscpu.h).
 */
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <asm/klausscpu.h>
#include <asm/setup.h>

bool klausscpu_has_l2(void)
{
	return klausscpu_cpu_features & CPU_FEAT_L2;
}

static void __iomem *l2_ctrl(void)
{
	return (void __iomem *)KL_L2_CTRL;
}

static int l2_show(struct seq_file *m, void *v)
{
	seq_printf(m, "l2 %llu\n", readq(l2_ctrl()) & KL_L2_CTRL_EN);
	return 0;
}

static int l2_open(struct inode *inode, struct file *file)
{
	return single_open(file, l2_show, NULL);
}

static ssize_t l2_write(struct file *file, const char __user *buf,
			size_t len, loff_t *pos)
{
	char c;
	int i;

	if (!len || get_user(c, buf))
		return -EFAULT;
	if (c != '0' && c != '1')
		return -EINVAL;
	writeq(c == '1' ? KL_L2_CTRL_EN : 0, l2_ctrl());
	/* Turning it off writes the dirty lines back first. */
	for (i = 0; i < 100000 && (readq(l2_ctrl()) & KL_L2_CTRL_BUSY); i++)
		udelay(1);
	if (readq(l2_ctrl()) & KL_L2_CTRL_BUSY)
		return -EIO;
	return len;
}

static const struct proc_ops l2_ops = {
	.proc_open	= l2_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
	.proc_write	= l2_write,
};

static int __init l2_init(void)
{
	u64 info;

	if (!klausscpu_has_l2())
		return 0;
	info = readq((void __iomem *)KL_L2_INFO);
	pr_info("KlaussCPU: L2 %llu KiB, %llu-way, %llu B lines, %s\n",
		(info >> 32) / 1024, info & 0xff, (info >> 24) & 0xff,
		readq(l2_ctrl()) & KL_L2_CTRL_EN ? "on" : "off");
	proc_create("klausscpu_l2", 0644, NULL, &l2_ops);
	return 0;
}
device_initcall(l2_init);

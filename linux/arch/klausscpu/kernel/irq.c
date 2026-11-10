// SPDX-License-Identifier: GPL-2.0
/*
 * The interrupt controller: 4 sources, per-source vectors INT_VECn.  As in
 * the Zephyr port, INT_MASK stays 0xF for good and a source is enabled by
 * pointing INT_VECn at its stub (0 = disabled), so frames never carry a
 * stale mask for IRET to restore.  Entry auto-masks the dispatched source
 * in INT_MASK and IRET restores it.
 *
 * Source 2 is shared by Ethernet and, on bitstreams with the native SD
 * controller (CPU_FEATURES[21]), the SD card: INT_PENDING[2] =
 * |(INT2_PENDING & INT2_EN).  There it is a chained irqchip with two
 * children, masked through INT2_EN.  Older bitstreams read INT2_* as 0, so
 * without the feature bit source 2 stays Ethernet's own line.
 */
#include <linux/bitops.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/io.h>
#include <asm/klausscpu.h>
#include <asm/setup.h>

extern void klausscpu_irq_vector0(void), klausscpu_irq_vector1(void),
	    klausscpu_irq_vector2(void), klausscpu_irq_vector3(void);

static void (*const kl_vectors[KL_NR_IRQS])(void) = {
	klausscpu_irq_vector0, klausscpu_irq_vector1,
	klausscpu_irq_vector2, klausscpu_irq_vector3,
};

static struct irq_domain *kl_domain;

static void __iomem *intc(unsigned int off)
{
	return (void __iomem *)(KL_INTC_BASE + off);
}

static void kl_irq_mask(struct irq_data *d)
{
	writel(0, intc(KL_INT_VEC(d->hwirq)));
}

static void kl_irq_unmask(struct irq_data *d)
{
	writel((unsigned long)kl_vectors[d->hwirq], intc(KL_INT_VEC(d->hwirq)));
}

static struct irq_chip kl_irq_chip = {
	.name		= "klausscpu-intc",
	.irq_mask	= kl_irq_mask,
	.irq_unmask	= kl_irq_unmask,
};

static int kl_irq_map(struct irq_domain *d, unsigned int irq, irq_hw_number_t hw)
{
	irq_set_chip_and_handler(irq, &kl_irq_chip, handle_level_irq);
	return 0;
}

static const struct irq_domain_ops kl_irq_ops = {
	.map	= kl_irq_map,
	.xlate	= irq_domain_xlate_onecell,
};

#define KL_INT2_NR	2

static struct irq_domain *kl_int2_domain;
static DEFINE_RAW_SPINLOCK(kl_int2_lock);

static void kl_int2_set(unsigned int hw, bool on)
{
	unsigned long flags;
	u32 en;

	raw_spin_lock_irqsave(&kl_int2_lock, flags);
	en = readl(intc(KL_INT2_EN));
	en = on ? en | BIT(hw) : en & ~BIT(hw);
	writel(en, intc(KL_INT2_EN));
	raw_spin_unlock_irqrestore(&kl_int2_lock, flags);
}

static void kl_int2_mask(struct irq_data *d)
{
	kl_int2_set(d->hwirq, false);
}

static void kl_int2_unmask(struct irq_data *d)
{
	kl_int2_set(d->hwirq, true);
}

static struct irq_chip kl_int2_chip = {
	.name		= "klausscpu-int2",
	.irq_mask	= kl_int2_mask,
	.irq_unmask	= kl_int2_unmask,
};

static int kl_int2_map(struct irq_domain *d, unsigned int irq, irq_hw_number_t hw)
{
	irq_set_chip_and_handler(irq, &kl_int2_chip, handle_level_irq);
	return 0;
}

static const struct irq_domain_ops kl_int2_ops = {
	.map	= kl_int2_map,
	.xlate	= irq_domain_xlate_onecell,
};

static void kl_int2_handler(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	unsigned long pend;
	unsigned int hw;

	chained_irq_enter(chip, desc);
	pend = readl(intc(KL_INT2_PEND)) & readl(intc(KL_INT2_EN)) & (BIT(KL_INT2_NR) - 1);
	for_each_set_bit(hw, &pend, KL_INT2_NR)
		generic_handle_domain_irq(kl_int2_domain, hw);
	chained_irq_exit(chip, desc);
}

unsigned int klausscpu_int2_irq(unsigned int child)
{
	if (kl_int2_domain)
		return irq_create_mapping(kl_int2_domain, child);
	return child == KL_INT2_ETH ? klausscpu_irq(2) : 0;
}

static void __init kl_int2_init(void)
{
	if (!(klausscpu_cpu_features & CPU_FEAT_SDNATIVE))
		return;
	writel(0, intc(KL_INT2_EN));	/* children unmask when requested */
	kl_int2_domain = irq_domain_create_linear(NULL, KL_INT2_NR, &kl_int2_ops, NULL);
	if (!kl_int2_domain)
		panic("klausscpu: no IRQ domain for source 2");
	irq_set_chained_handler_and_data(klausscpu_irq(2), kl_int2_handler, NULL);
	pr_info("klausscpu-intc: source 2 cascades Ethernet and SD\n");
}

void klausscpu_handle_irq(unsigned int hwirq)
{
	generic_handle_domain_irq(kl_domain, hwirq);
}

unsigned int klausscpu_irq(unsigned int hwirq)
{
	return irq_create_mapping(kl_domain, hwirq);
}

void __init init_IRQ(void)
{
	int i;

	for (i = 0; i < KL_NR_IRQS; i++)
		writel(0, intc(KL_INT_VEC(i)));
	writel(0xf, intc(KL_INT_MASK));
	kl_domain = irq_domain_create_linear(NULL, KL_NR_IRQS, &kl_irq_ops, NULL);
	if (!kl_domain)
		panic("klausscpu: no IRQ domain");
	kl_int2_init();
}

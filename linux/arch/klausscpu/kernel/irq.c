// SPDX-License-Identifier: GPL-2.0
/*
 * The interrupt controller: 4 sources, per-source vectors INT_VECn.  As in
 * the Zephyr port, INT_MASK stays 0xF for good and a source is enabled by
 * pointing INT_VECn at its stub (0 = disabled), so frames never carry a
 * stale mask for IRET to restore.  Entry auto-masks the dispatched source
 * in INT_MASK and IRET restores it.
 */
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
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
}

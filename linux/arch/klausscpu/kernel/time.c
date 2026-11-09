// SPDX-License-Identifier: GPL-2.0
/*
 * clocksource and sched_clock: CYCLE (64-bit, the core clock).
 * clockevent: the periodic interval timer (source 0); TIMER_PER is its
 * period in timer clocks, and dispatch clears its pending bit.
 */
#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/sched_clock.h>
#include <asm/klausscpu.h>

extern unsigned int klausscpu_irq(unsigned int hwirq);

static u64 kl_read_cycles(struct clocksource *cs)
{
	return mfsr(SR_CYCLE);
}

static u64 notrace kl_sched_clock(void)
{
	return mfsr(SR_CYCLE);
}

static struct clocksource kl_clocksource = {
	.name	= "klausscpu-cycle",
	.rating	= 300,
	.read	= kl_read_cycles,
	.mask	= CLOCKSOURCE_MASK(64),
	.flags	= CLOCK_SOURCE_IS_CONTINUOUS,
};

static void __iomem *timer_per(void)
{
	return (void __iomem *)(KL_INTC_BASE + KL_TIMER_PER);
}

static int kl_timer_set_periodic(struct clock_event_device *ced)
{
	writel(CONFIG_KLAUSSCPU_TIMER_HZ / HZ, timer_per());
	return 0;
}

static int kl_timer_shutdown(struct clock_event_device *ced)
{
	writel(0, timer_per());
	return 0;
}

static struct clock_event_device kl_clockevent = {
	.name			= "klausscpu-timer",
	.features		= CLOCK_EVT_FEAT_PERIODIC,
	.rating			= 300,
	.set_state_periodic	= kl_timer_set_periodic,
	.set_state_shutdown	= kl_timer_shutdown,
};

static irqreturn_t kl_timer_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *ced = dev_id;

	ced->event_handler(ced);
	return IRQ_HANDLED;
}

void __init time_init(void)
{
	unsigned int irq = klausscpu_irq(KL_IRQ_TIMER);

	clocksource_register_hz(&kl_clocksource, CONFIG_KLAUSSCPU_CPU_HZ);
	sched_clock_register(kl_sched_clock, 64, CONFIG_KLAUSSCPU_CPU_HZ);

	kl_clockevent.cpumask = cpumask_of(0);
	kl_clockevent.irq = irq;
	if (request_irq(irq, kl_timer_interrupt, IRQF_TIMER, "timer", &kl_clockevent))
		panic("klausscpu: timer IRQ");
	clockevents_register_device(&kl_clockevent);
}

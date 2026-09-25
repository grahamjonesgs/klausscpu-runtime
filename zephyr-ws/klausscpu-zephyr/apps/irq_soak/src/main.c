/*
 * irq_soak — on-silicon soak for the pipeline's irq_lock dispatch race.
 *
 * Hardware background (pipeline_core.sv, fixed 2026-09-25): the core decides
 * to take an interrupt at a dispatch boundary, then drains the pipeline
 * before pushing the frame. Pre-fix, an OLDER in-flight INT_MASK store
 * (exactly arch_irq_lock's `_KLAUSSCPU_INT_MASK = 0`) landed during that
 * drain and the frame was pushed anyway — the ISR ran inside the critical
 * section, together with one or more section-body instructions that had
 * already dispatched behind the in-flight store. The fix re-checks
 * irq_ready at the drain boundary and abandons the entry.
 *
 * Two independent oracles, both zero-false-positive by in-order retirement:
 *
 *  1. g_in_crit flag: set to 1 by the FIRST store after irq_lock() (it
 *     dispatches right behind the mask store, so in the racy dispatch it
 *     drains with the pipeline and the ISR observes it), cleared before
 *     irq_unlock(). The tick ISR must NEVER see it as 1: the flag's set
 *     retires only after the lock store (in-order), and from that retire
 *     until after the clear retires the mask is 0 at every legal dispatch
 *     boundary. Any ISR observation of 1 == handler inside the lock.
 *
 *  2. Lost-update counter: g_shared is incremented under the lock via a
 *     deliberately widened read-spin-write, and by the ISR outside it.
 *     Every increment is accounted (expected = loops + ticks); an ISR
 *     increment landing inside the read..write window is overwritten and
 *     shows up as expected - g_shared > 0.
 *
 * A churn thread hammers k_sleep/k_sem so the kernel's own irq_lock-protected
 *  timeout/ready queues soak too — pre-fix corruption there shows up as a
 * hang or fatal error rather than a counter.
 *
 * Output: heartbeat line every ~5 s; any violation is reported immediately.
 * The host side decides the soak length and greps the heartbeats.
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

static volatile uint32_t g_in_crit;      /* 1 while inside the critical section */
static volatile uint32_t g_viol_flag;    /* ISR saw g_in_crit == 1              */
static volatile uint32_t g_shared;       /* two-writer counter (lock-protected)  */
static volatile uint32_t g_ticks;        /* ISR invocations                      */
static volatile uint64_t g_loops;        /* completed lock/unlock iterations     */
static volatile uint32_t g_viol_last_loop;

static void tick_fn(struct k_timer *t)
{
	ARG_UNUSED(t);
	g_ticks++;
	if (g_in_crit) {
		g_viol_flag++;
		g_viol_last_loop = (uint32_t)g_loops;
	}
	g_shared++;                       /* legal only OUTSIDE the lock */
}
K_TIMER_DEFINE(tick_timer, tick_fn, NULL);

/* Churn thread: keeps the scheduler + timeout list busy across ticks. */
K_SEM_DEFINE(churn_sem, 0, 1);
static void churn_main(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	for (;;) {
		(void)k_sem_take(&churn_sem, K_MSEC(2));
		k_sleep(K_MSEC(1));
	}
}
K_THREAD_DEFINE(churn_tid, 2048, churn_main, NULL, NULL, NULL, 7, 0, 0);

int main(void)
{
	int64_t next_beat = 5000;

	printk("IRQSOAK START (ticks=1kHz, oracle=in-crit flag + lost-update)\n");
	k_timer_start(&tick_timer, K_MSEC(1), K_MSEC(1));

	for (;;) {
		unsigned int key = irq_lock();
		g_in_crit = 1;               /* first store behind the mask store */
		/* widened critical section: read-spin-write on the shared ctr */
		{
			uint32_t tmp = g_shared;
			for (volatile int i = 0; i < 8; i++) {
			}
			g_shared = tmp + 1u;
		}
		g_in_crit = 0;
		irq_unlock(key);
		g_loops++;

		if ((g_loops & 0x3FF) == 0) {  /* breathe: let ticks/churn run */
			int64_t now = k_uptime_get();

			if (now >= next_beat) {
				next_beat += 5000;
				uint32_t exp = (uint32_t)g_loops + g_ticks;
				printk("BEAT t=%lld s loops=%llu ticks=%u viol=%u lost=%d\n",
				       now / 1000, g_loops, g_ticks,
				       g_viol_flag, (int32_t)(exp - g_shared));
				if (g_viol_flag) {
					printk("VIOLATION: ISR ran inside irq_lock "
					       "(count=%u, last at loop=%u)\n",
					       g_viol_flag, g_viol_last_loop);
				}
			}
		}
	}
	return 0;
}

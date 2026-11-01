/*
 * irq_dev — device interrupts through the Zephyr interrupt API, on silicon.
 *
 * The KlaussCPU SoC has four interrupt sources: 0 timer (the kernel tick),
 * 1 blitter DONE, 2 Ethernet, 3 VGA vsync.  This app uses two real devices:
 *
 *   VGA vsync  (IRQ 3)  connected STATICALLY with IRQ_CONNECT(): the controller
 *                       raises it once per frame (59.5 Hz), so its rate is the
 *                       oracle; the controller's own frame counter
 *                       (VGA_STATUS[31:16]) counts frames regardless of whether
 *                       the CPU took the interrupt, so ISR count == frame count
 *                       proves none was lost.
 *   blitter    (IRQ 1)  connected DYNAMICALLY with irq_connect_dynamic(): each
 *                       FILL blit of a small rectangle raises DONE when it
 *                       completes, so N blits must give exactly N interrupts.
 *
 * Phases (each prints one "IRQDEV ..." line and the app ends with
 * "IRQDEV RESULT PASS" or "IRQDEV RESULT FAIL"):
 *
 *   A  vsync alone for 2 s: ISR count equals the frame counter's delta and
 *      the rate is 59.5 Hz +-1.
 *   B  2000 blits, one at a time, each waiting for its interrupt through a
 *      semaphore the ISR gives: 2000 interrupts, no timeouts.  Each wakes the
 *      waiting thread, so this also takes the ISR-exit reschedule path.
 *   C  the same two interrupts under load, irq_soak's lock oracles included
 *      (a handler must never run inside irq_lock(); no lost update on a
 *      shared counter, since every handler increments it outside the lock),
 *      a churn thread on k_sleep/k_sem, a k_timer, and the 1 kHz tick:
 *        C1 (5 s)  eight blits per millisecond; the soak thread runs in what
 *                  is left of the CPU; vsync count == frame count, blit ISR
 *                  count == blits issued, no handler inside the lock;
 *        C2 (5 s)  a tight blit/wait loop (about 30k interrupts per second,
 *                  too fast for any lower-priority thread to get a word in);
 *                  the same counts.
 *   D  API semantics: arch_irq_disable() stops delivery, irq_connect_dynamic()
 *      refuses the tick (-EINVAL) and an enabled source (-EBUSY), a vsync
 *      handler can be disconnected and replaced dynamically.
 *   E  the ISR stack's high-water mark from the thread analyzer ("ISR0":
 *      OPEN_ITEMS 18 -- the handlers run on z_interrupt_stacks).
 *
 * VGA scan-out is not needed (the vsync timing runs regardless) and the app
 * leaves the controller as it found it (VGA_CTRL = 0x8, test pattern).
 *
 * CONFIG_IRQ_DEV_SPURIOUS replaces the test with the negative case (an enabled
 * source with no handler must end in "spurious interrupt"), and
 * CONFIG_IRQ_DEV_BAD_CONNECT must not build (the tick cannot be connected).
 *
 * The emulator does not model sources 1..3: there the app reports that no
 * vsync arrived and stops.
 */
#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <zephyr/init.h>
#include <zephyr/sys/printk.h>
#include <zephyr/debug/thread_analyzer.h>
#include <errno.h>

#define MMIO(a)        (*(volatile uint64_t *)(uintptr_t)(a))
#define VGA_CTRL       MMIO(0xF0110000u)
#define VGA_STATUS     MMIO(0xF0110018u)
#define VGA_CTRL_TEST_PATTERN (1u << 3)
#define VGA_CTRL_VSYNC_IRQ_EN (1u << 4)
#define VGA_STATUS_VSYNC      (1u << 1)   /* W1C */
#define VGA_FRAME()    ((uint32_t)((VGA_STATUS >> 16) & 0xFFFFu))

#define BLIT_BASE      0xF00E0000u
#define BLIT_CTRL      MMIO(BLIT_BASE + 0x00u)
#define BLIT_STATUS    MMIO(BLIT_BASE + 0x08u)
#define BLIT_DST_ADDR  MMIO(BLIT_BASE + 0x10u)
#define BLIT_DST_STRIDE MMIO(BLIT_BASE + 0x18u)
#define BLIT_WIDTH     MMIO(BLIT_BASE + 0x30u)
#define BLIT_HEIGHT    MMIO(BLIT_BASE + 0x38u)
#define BLIT_COLOR     MMIO(BLIT_BASE + 0x40u)
#define BLIT_START_FILL_IRQ (1u | (0u << 1) | (1u << 4))   /* START | OP=FILL | IRQ_EN */
#define BLIT_STATUS_DONE    (1u << 1)                       /* W1C */

#define IRQ_BLIT   1
#define IRQ_VGA    3

static volatile uint32_t g_vsync;        /* vsync ISR invocations                   */
static volatile uint32_t g_blit_isr;     /* blitter ISR invocations                 */
static volatile uint32_t g_in_crit;      /* 1 while inside the soak's critical section */
static volatile uint32_t g_viol;         /* an ISR saw g_in_crit == 1               */
static volatile uint32_t g_shared;       /* two-writer counter (lock-protected)     */
static volatile uint32_t g_ticks;
static volatile uint32_t g_vsync2;       /* the replacement vsync handler (phase D) */
static bool g_failed;

K_SEM_DEFINE(blit_sem, 0, 1);

static uint16_t blit_buf[32 * 32] __aligned(32);

/* ── handlers ──────────────────────────────────────────────────────────────
 * Level sources: acknowledge at the device before returning. */
static void vsync_isr(const void *arg)
{
	VGA_STATUS = VGA_STATUS_VSYNC;
	g_vsync++;
	if (g_in_crit) {
		g_viol++;
	}
	g_shared++;                      /* legal only OUTSIDE the lock */
	ARG_UNUSED(arg);
}

static void vsync2_isr(const void *arg)
{
	VGA_STATUS = VGA_STATUS_VSYNC;
	g_vsync2 += (uint32_t)(uintptr_t)arg;
}

static void blit_isr(const void *arg)
{
	BLIT_STATUS = BLIT_STATUS_DONE;
	g_blit_isr++;
	if (g_in_crit) {
		g_viol++;
	}
	g_shared++;
	k_sem_give((struct k_sem *)arg);
}

/* Static connection of the vsync handler (before main(), at init). */
static int connect_static(void)
{
	IRQ_CONNECT(IRQ_VGA, 0, vsync_isr, NULL, 0);
	return 0;
}
SYS_INIT(connect_static, APPLICATION, 50);

#ifdef CONFIG_IRQ_DEV_BAD_CONNECT
static int connect_bad(void)
{
	IRQ_CONNECT(0, 0, vsync_isr, NULL, 0);   /* the tick: refused at build time */
	return 0;
}
SYS_INIT(connect_bad, APPLICATION, 51);
#endif

static void check(const char *what, bool ok)
{
	printk("%s irq_dev: %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok) {
		g_failed = true;
	}
}

static void blit_once(void)
{
	BLIT_DST_ADDR = (uintptr_t)blit_buf;
	BLIT_DST_STRIDE = 64;
	BLIT_WIDTH = 32;
	BLIT_HEIGHT = 8;
	BLIT_COLOR = 0xF81F;
	BLIT_CTRL = BLIT_START_FILL_IRQ;
}

static void vsync_on(void)
{
	VGA_STATUS = VGA_STATUS_VSYNC;           /* drop a stale pending */
	VGA_CTRL = VGA_CTRL_TEST_PATTERN | VGA_CTRL_VSYNC_IRQ_EN;
	irq_enable(IRQ_VGA);
}

static void vsync_off(void)
{
	irq_disable(IRQ_VGA);
	VGA_CTRL = VGA_CTRL_TEST_PATTERN;
	VGA_STATUS = VGA_STATUS_VSYNC;
}

/* ── load for phase C ─────────────────────────────────────────────────── */
static volatile bool soak_run;
static volatile int blit_mode;           /* 0 idle, 1 bursts of 8 per ms, 2 tight loop */
static volatile uint64_t g_loops;
static volatile uint32_t g_blits_issued, g_blit_timeouts;

static void tick_fn(struct k_timer *t)
{
	ARG_UNUSED(t);
	g_ticks++;
	if (g_in_crit) {
		g_viol++;
	}
	g_shared++;
}
K_TIMER_DEFINE(tick_timer, tick_fn, NULL);

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

/* Blit and wait for the interrupt. */
static void blit_main(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	for (;;) {
		int mode = blit_mode;

		if (mode == 0) {
			k_sleep(K_MSEC(5));
			continue;
		}
		for (int i = 0; i < (mode == 1 ? 8 : 64); i++) {
			k_sem_reset(&blit_sem);
			blit_once();
			g_blits_issued++;
			if (k_sem_take(&blit_sem, K_MSEC(100)) != 0) {
				g_blit_timeouts++;
			}
		}
		if (mode == 1) {
			k_sleep(K_MSEC(1));
		}
	}
}
K_THREAD_DEFINE(blit_tid, 2048, blit_main, NULL, NULL, NULL, 3, 0, 0);

/* irq_soak's critical-section oracles, in a low-priority thread so that the
 * interrupts' wake-ups preempt it at ISR exit. */
static void soak_main(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	for (;;) {
		while (!soak_run) {
			k_sleep(K_MSEC(5));
		}
		unsigned int key = irq_lock();

		g_in_crit = 1;
		{
			uint32_t tmp = g_shared;

			for (volatile int i = 0; i < 8; i++) {
			}
			g_shared = tmp + 1u;
		}
		g_in_crit = 0;
		irq_unlock(key);
		g_loops++;
	}
}
K_THREAD_DEFINE(soak_tid, 2048, soak_main, NULL, NULL, NULL, 10, 0, 0);

int main(void)
{
	printk("IRQDEV START (vsync static IRQ 3, blitter dynamic IRQ 1)\n");

#ifdef CONFIG_IRQ_DEV_SPURIOUS
	/* IRQ 1 never connected: enabling it and raising DONE must be fatal. */
	printk("IRQDEV spurious test: enabling IRQ 1 with no handler\n");
	irq_enable(IRQ_BLIT);
	blit_once();
	k_sleep(K_MSEC(100));
	printk("IRQDEV RESULT FAIL (no spurious interrupt)\n");
	return 0;
#endif

	/* Is there a VGA controller that raises vsync? (Not in the emulator.) */
	uint32_t f0 = VGA_FRAME();

	k_sleep(K_MSEC(100));
	if (VGA_FRAME() == f0) {
		printk("IRQDEV no VGA frame counter running (emulator?); stopping\n");
		return 0;
	}

	/* ── A: vsync, statically connected ──────────────────────────────── */
	check("IRQ 3 starts disabled", !irq_is_enabled(IRQ_VGA));
	g_vsync = 0;
	uint32_t fa = VGA_FRAME();
	int64_t ta = k_uptime_get();

	vsync_on();
	k_sleep(K_MSEC(2000));
	vsync_off();
	uint32_t va = g_vsync, frames = (VGA_FRAME() - fa) & 0xFFFFu;
	uint32_t ms = (uint32_t)(k_uptime_get() - ta);
	uint32_t milli_hz = (uint32_t)(((uint64_t)va * 1000000u) / ms);

	printk("IRQDEV vsync: isr=%u frames=%u in %u ms = %u.%03u Hz\n",
	       va, frames, ms, milli_hz / 1000u, milli_hz % 1000u);
	check("vsync: no interrupt lost (ISR count == frame counter, +-1)",
	      va + 1u >= frames && va <= frames + 1u);
	check("vsync: 59.5 Hz +-1", milli_hz >= 58500u && milli_hz <= 60500u);

	/* ── B: blitter, dynamically connected, N blits ──────────────────── */
	int rc = irq_connect_dynamic(IRQ_BLIT, 0, blit_isr, &blit_sem, 0);

	check("irq_connect_dynamic(1) returns the IRQ number", rc == IRQ_BLIT);
	g_blit_isr = 0;
	BLIT_STATUS = BLIT_STATUS_DONE;
	irq_enable(IRQ_BLIT);

	uint32_t timeouts = 0;

	for (int i = 0; i < 2000; i++) {
		k_sem_reset(&blit_sem);
		blit_once();
		if (k_sem_take(&blit_sem, K_MSEC(100)) != 0) {
			timeouts++;
		}
	}
	printk("IRQDEV blitter: isr=%u timeouts=%u (2000 blits)\n", g_blit_isr, timeouts);
	check("blitter: 2000 blits give 2000 interrupts", g_blit_isr == 2000u && timeouts == 0u);

	/* ── C: both under load ──────────────────────────────────────────── */
	for (int phase = 1; phase <= 2; phase++) {
		g_vsync = 0; g_blit_isr = 0; g_ticks = 0; g_shared = 0; g_loops = 0;
		g_viol = 0; g_blits_issued = 0; g_blit_timeouts = 0;
		uint32_t fc = VGA_FRAME();

		k_timer_start(&tick_timer, K_MSEC(1), K_MSEC(1));
		vsync_on();
		blit_mode = phase;
		soak_run = (phase == 1);
		k_sleep(K_MSEC(5000));
		soak_run = false;
		blit_mode = 0;
		k_sleep(K_MSEC(20));       /* let the last blit and soak iteration finish */
		vsync_off();
		k_timer_stop(&tick_timer);
		uint32_t vc = g_vsync, framesc = (VGA_FRAME() - fc) & 0xFFFFu;
		uint32_t expect = (uint32_t)g_loops + g_ticks + g_vsync + g_blit_isr;
		int32_t lost = (int32_t)(expect - g_shared);

		printk("IRQDEV C%d: loops=%llu ticks=%u vsync=%u/%u blit_isr=%u issued=%u timeouts=%u viol=%u lost=%d\n",
		       phase, g_loops, g_ticks, vc, framesc, g_blit_isr, g_blits_issued,
		       g_blit_timeouts, g_viol, lost);
		check("load: no handler ran inside irq_lock()", g_viol == 0u);
		check("load: no lost update", lost == 0);
		check("load: vsync count == frame counter (+-1)",
		      vc + 1u >= framesc && vc <= framesc + 1u);
		check("load: every blit raised exactly one interrupt",
		      g_blit_isr == g_blits_issued && g_blit_timeouts == 0u && g_blits_issued > 1000u);
		check("load: ticks kept coming (1 kHz, +-2%)", g_ticks >= 4900u && g_ticks <= 5100u);
		if (phase == 1) {
			check("load: the soak thread made progress", g_loops > 10000u);
		}
	}

	/* ── D: API semantics ─────────────────────────────────────────────── */
	check("irq_connect_dynamic(0) refused (the tick)",
	      irq_connect_dynamic(0, 0, vsync2_isr, NULL, 0) == -EINVAL);
	check("irq_connect_dynamic(7) refused (no such source)",
	      irq_connect_dynamic(7, 0, vsync2_isr, NULL, 0) == -EINVAL);

	vsync_on();
	check("irq_is_enabled(3) after irq_enable", irq_is_enabled(IRQ_VGA));
	check("irq_connect_dynamic on an enabled source refused",
	      irq_connect_dynamic(IRQ_VGA, 0, vsync2_isr, NULL, 0) == -EBUSY);
	irq_disable(IRQ_VGA);
	check("irq_is_enabled(3) after irq_disable", !irq_is_enabled(IRQ_VGA));
	uint32_t v0 = g_vsync;

	k_sleep(K_MSEC(300));          /* VSYNC_IRQ_EN still on: pending, not delivered */
	check("irq_disable: no delivery for 300 ms (18 frames)", g_vsync == v0);
	irq_enable(IRQ_VGA);
	k_sleep(K_MSEC(200));
	check("irq_enable: delivery resumes (pending level taken)", g_vsync > v0);
	irq_disable(IRQ_VGA);

	check("irq_disconnect_dynamic with the wrong handler refused",
	      irq_disconnect_dynamic(IRQ_VGA, 0, vsync2_isr, NULL, 0) == -EINVAL);
	check("irq_disconnect_dynamic of the static handler",
	      irq_disconnect_dynamic(IRQ_VGA, 0, vsync_isr, NULL, 0) == 0);
	check("irq_connect_dynamic(3) with a replacement handler",
	      irq_connect_dynamic(IRQ_VGA, 0, vsync2_isr, (void *)(uintptr_t)3, 0) == IRQ_VGA);
	v0 = g_vsync;
	g_vsync2 = 0;
	vsync_on();
	k_sleep(K_MSEC(1000));
	vsync_off();
	printk("IRQDEV replacement handler: %u (3 per vsync), old handler: %u\n",
	       g_vsync2, g_vsync - v0);
	check("replacement handler runs, old one does not",
	      g_vsync2 >= 3u * 58u && g_vsync2 <= 3u * 62u && g_vsync == v0);
	irq_disconnect_dynamic(IRQ_VGA, 0, vsync2_isr, (void *)(uintptr_t)3, 0);
	irq_disable(IRQ_BLIT);
	irq_disconnect_dynamic(IRQ_BLIT, 0, blit_isr, &blit_sem, 0);

	/* ── E: ISR stack high-water mark ─────────────────────────────────── */
		thread_analyzer_print();

	/* The controller as we found it. */
	VGA_CTRL = VGA_CTRL_TEST_PATTERN;

	printk("IRQDEV RESULT %s\n", g_failed ? "FAIL" : "PASS");
	return 0;
}

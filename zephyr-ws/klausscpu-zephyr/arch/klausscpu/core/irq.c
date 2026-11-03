/*
 * irq.c — KlaussCPU IRQ management for Zephyr.
 *
 * KlaussCPU has a flat MMIO interrupt controller at 0xF00F_0000 (CPU_ARCHITECTURE.md 13.1,
 * MMIO_MAP.md):
 *   INT_MASK    bit n = source n (4 bits).  The core clears the dispatched
 *               source's bit on entry and IRET restores the whole mask from the
 *               saved frame.
 *   INT_PENDING live pending bits.
 *   INT_VECn    byte address of source n's handler; 0 disables the source even
 *               when its INT_MASK bit is set.
 * A source is dispatched when INT_MASK[n] & INT_PENDING[n] & (INT_VECn != 0).
 *
 * Zephyr IRQ numbers are the source numbers (arch/klausscpu/irq.h): 0 = timer,
 * 1 = blitter DONE, 2 = Ethernet, 3 = VGA vsync.
 *
 * Enabling and disabling.  The port keeps INT_MASK at 0xF for good and
 * arch_irq_enable()/arch_irq_disable() write INT_VECn (the stub's address / 0).
 * The hardware saves the mask in every interrupt frame and IRET restores it,
 * and arch_switch() and arch_new_thread() build frames of their own, so a
 * mask that changed after a thread's frame was written would be reverted when
 * that thread resumes.  With the mask constant a frame can never be stale, and
 * the enable state lives in one place (INT_VEC, read back by
 * arch_irq_is_enabled()).  The stubs clear the dispatched source's bit while
 * they run; IRET puts it back.
 *
 * Handlers.  INT_VECn points at a stub in swap.S (z_klausscpu_timer_isr for
 * source 0, z_klausscpu_isrN otherwise).  The stub saves all registers on the
 * interrupted stack, executes DI, switches to the ISR stack (Zephyr's
 * z_interrupt_stacks, CONFIG_ISR_STACK_SIZE) and calls the handler from
 * _sw_isr_table[n] (for the timer, sys_clock_isr() directly).  So a handler
 * runs to completion with interrupts disabled, one at a time: there is no
 * nesting and no priority.  When several sources are pending the hardware
 * picks timer > blitter > Ethernet > VGA.  After the handler the stub resumes
 * the highest-priority ready thread (klausscpu_isr.h), then IRETs.
 *
 * A level-triggered source (1..3) must be acknowledged at the device before
 * its handler returns; IRET restores its INT_MASK bit and a still-asserted
 * level dispatches the handler again at once.
 *
 * Connecting.  IRQ_CONNECT() (static) puts a record in the .klausscpu_irq_conn
 * section; klausscpu_irq_init() copies the records into _sw_isr_table at boot.
 * arch_irq_connect_dynamic() (CONFIG_DYNAMIC_INTERRUPTS) writes the table at
 * run time.  Source 0 belongs to the kernel's tick and cannot be connected.
 * A source without a connection has INT_VEC = 0 until arch_irq_enable(); its
 * table entry is z_irq_spurious(), so enabling it by mistake ends in a fatal
 * K_ERR_SPURIOUS_IRQ, not in silence or a storm.
 */

#include <zephyr/kernel.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/irq.h>
#include <zephyr/init.h>
#include <zephyr/sw_isr_table.h>
#include <zephyr/fatal.h>

#include <errno.h>
#include <string.h>

#include "klausscpu_isr.h"

/* Declared in zephyr/lib/libc/minimal/source/stdout/stdout_console.c.
 * Not exposed via any public header, so forward-declare here exactly
 * as the other console drivers (uart_console.c, ram_console.c, …) do. */
extern void __stdout_hook_install(int (*hook)(int c));

/* MMIO shortcuts (no mmio.h dependency to keep arch self-contained). */
#define _INT_MASK   (*(volatile uint32_t *)0xF00F0000u)
#define _INT_PEND   (*(volatile uint32_t *)0xF00F0008u)
#define _INT_VEC(n) (*(volatile uint32_t *)(uintptr_t)(0xF00F0010u + 8u * (n)))

/* UART (0xF001_0000): TX write transmits low byte, STATUS bit0 = TX busy. */
#define _UART_TX      (*(volatile uint32_t *)0xF0010000u)
#define _UART_STATUS  (*(volatile uint32_t *)0xF0010010u)
#define _UART_TX_BUSY (1u << 0)

/* "In ISR" flag — read by arch_is_in_isr().  Set by the stubs (there is no
 * nesting: they run with interrupts disabled), cleared by them or by
 * arch_switch() when it leaves an ISR for another thread. */
volatile uint32_t _klausscpu_in_isr;

/* Set once the first thread exists (APPLICATION, below): until then an ISR
 * must not look at the ready queue (klausscpu_isr.h). */
volatile uint32_t _klausscpu_sched_ready;

/* ISR-exit scratch: the interrupted thread's saved SP, kept across the switch
 * to the ISR stack so z_klausscpu_isr_exit() can hand it to
 * z_get_next_switch_handle() once the handler has returned. */
uint64_t _klausscpu_isr_handle;

/* The ISR stack (kernel/init.c): _kernel.cpus[0].irq_stack is its top. */
K_KERNEL_STACK_ARRAY_DECLARE(z_interrupt_stacks, CONFIG_MP_MAX_NUM_CPUS, CONFIG_ISR_STACK_SIZE);

/* Per-source handler and argument; the entry for the timer is unused. */
struct _isr_table_entry _sw_isr_table[KLAUSSCPU_NUM_IRQS];

/* The entry stubs in swap.S, by source. */
extern void z_klausscpu_timer_isr(void);
extern void z_klausscpu_isr1(void);
extern void z_klausscpu_isr2(void);
extern void z_klausscpu_isr3(void);
static void (*const _isr_stub[KLAUSSCPU_NUM_IRQS])(void) = {
    z_klausscpu_timer_isr, z_klausscpu_isr1, z_klausscpu_isr2, z_klausscpu_isr3,
};

/* Bounds of the IRQ_CONNECT() records (soc linker.ld). */
extern const struct klausscpu_irq_conn __klausscpu_irq_conn_start[];
extern const struct klausscpu_irq_conn __klausscpu_irq_conn_end[];

void arch_irq_enable(unsigned int irq)
{
    __ASSERT(irq < KLAUSSCPU_NUM_IRQS, "KlaussCPU: no IRQ %u", irq);
    if (irq < KLAUSSCPU_NUM_IRQS) {
        _INT_VEC(irq) = (uint32_t)(uintptr_t)_isr_stub[irq];
    }
}

void arch_irq_disable(unsigned int irq)
{
    __ASSERT(irq < KLAUSSCPU_NUM_IRQS, "KlaussCPU: no IRQ %u", irq);
    if (irq < KLAUSSCPU_NUM_IRQS) {
        _INT_VEC(irq) = 0u;
    }
}

int arch_irq_is_enabled(unsigned int irq)
{
    return irq < KLAUSSCPU_NUM_IRQS && _INT_VEC(irq) != 0u;
}

/* The table entry of a source nobody connected: enabling it is a bug. */
void z_irq_spurious(const void *irq)
{
    printk("KlaussCPU: spurious interrupt, IRQ %u has no handler\n",
           (unsigned int)(uintptr_t)irq);
    z_fatal_error(K_ERR_SPURIOUS_IRQ, NULL);
}

/* Called by the z_klausscpu_isrN stubs (swap.S) on the ISR stack with
 * interrupts disabled, for sources 1..3. */
void z_klausscpu_irq_dispatch(unsigned int irq)
{
    const struct _isr_table_entry *e = &_sw_isr_table[irq];

    /* A stub entered before another stub's DI retired can resume after its
     * source was disabled or disconnected; that late call is dropped. */
    if (_INT_VEC(irq) != 0u) {
        e->isr(e->arg);
    }
    z_klausscpu_isr_exit();
}

static void irq_table_clear(unsigned int irq)
{
    _sw_isr_table[irq].arg = (const void *)(uintptr_t)irq;
    _sw_isr_table[irq].isr = z_irq_spurious;
}

/*
 * PRE_KERNEL_1, before any driver: every source off, the table to
 * "spurious", the static connections in, and INT_MASK to its permanent value.
 * Boot is still locked (DI), and no vector is set yet, so nothing can fire.
 */
static int klausscpu_irq_init(void)
{
    /* Paint the ISR stack for the thread analyzer's high-water mark (as arm
     * and riscv do at reset); no handler has run on it yet. */
    if (IS_ENABLED(CONFIG_INIT_STACKS)) {
        memset(K_KERNEL_STACK_BUFFER(z_interrupt_stacks[0]), 0xAA,
               K_KERNEL_STACK_SIZEOF(z_interrupt_stacks[0]));
    }

    for (unsigned int i = 0; i < KLAUSSCPU_NUM_IRQS; i++) {
        _INT_VEC(i) = 0u;
        irq_table_clear(i);
    }

    for (const struct klausscpu_irq_conn *c = __klausscpu_irq_conn_start;
         c < __klausscpu_irq_conn_end; c++) {
        /* ARCH_IRQ_CONNECT() checks the range at build time. */
        if (_sw_isr_table[c->irq].isr != z_irq_spurious) {
            printk("KlaussCPU: IRQ %u is connected twice\n", c->irq);
            k_panic();
        }
        _sw_isr_table[c->irq].arg = c->arg;
        _sw_isr_table[c->irq].isr = c->isr;
    }

    _INT_MASK = 0xFu;
    return 0;
}

SYS_INIT(klausscpu_irq_init, PRE_KERNEL_1, 0);

#ifdef CONFIG_DYNAMIC_INTERRUPTS
/* Connect a handler to a device source at run time.  Returns the IRQ number
 * (>= 0) or a negative errno.  The source must be disabled (Zephyr's rule for
 * dynamic connection); priority and flags are ignored. */
int arch_irq_connect_dynamic(unsigned int irq, unsigned int priority,
                             void (*routine)(const void *), const void *parameter,
                             uint32_t flags)
{
    ARG_UNUSED(priority);
    ARG_UNUSED(flags);

    if (irq == KLAUSSCPU_IRQ_TIMER || irq >= KLAUSSCPU_NUM_IRQS || routine == NULL) {
        return -EINVAL;
    }

    unsigned int key = arch_irq_lock();

    if (arch_irq_is_enabled(irq)) {
        arch_irq_unlock(key);
        return -EBUSY;
    }
    _sw_isr_table[irq].arg = parameter;
    _sw_isr_table[irq].isr = routine;
    arch_irq_unlock(key);
    return (int)irq;
}

/* Undo arch_irq_connect_dynamic() (or IRQ_CONNECT()): the source is disabled
 * and goes back to "spurious".  The routine and parameter must match. */
int arch_irq_disconnect_dynamic(unsigned int irq, unsigned int priority,
                                void (*routine)(const void *), const void *parameter,
                                uint32_t flags)
{
    ARG_UNUSED(priority);
    ARG_UNUSED(flags);

    if (irq == KLAUSSCPU_IRQ_TIMER || irq >= KLAUSSCPU_NUM_IRQS) {
        return -EINVAL;
    }

    unsigned int key = arch_irq_lock();
    int ret = 0;

    if (_sw_isr_table[irq].isr != routine || _sw_isr_table[irq].arg != parameter) {
        ret = -EINVAL;
    } else {
        arch_irq_disable(irq);
        irq_table_clear(irq);
    }
    arch_irq_unlock(key);
    return ret;
}
#endif /* CONFIG_DYNAMIC_INTERRUPTS */

/*
 * Override the weak default in zephyr/lib/os/printk.c so printk() actually
 * routes characters to our UART instead of being silently discarded.
 *
 * Zephyr normally wires this up via drivers/console/uart_console.c, which
 * calls __printk_hook_install(console_out) at SYS_INIT.  That driver isn't
 * being compiled in this build (no CONFIG_*_CONSOLE_SUBSYS dependency met),
 * so _char_out stays pointing at the weak no-op arch_printk_char_out.  By
 * supplying a strong arch_printk_char_out here we make printk work without
 * needing the full Zephyr console subsystem.
 *
 * Uses the same polled MMIO UART path as our UART driver — works from any
 * context, no kernel state required.
 */

/*
 * Optional console-output router.  When set, it is consulted for every
 * character printk()/minimal-libc printf() would send to the UART.  It returns
 * nonzero if it consumed the character (e.g. routed it to the SSH session of
 * the currently running extension, keyed off the running thread) and zero to
 * let it fall through to the UART.  This per-character, per-thread design lets
 * several extensions run concurrently, each routing its own stdout to its own
 * session, while kernel/log output on other threads still reaches the UART.
 * The hook owns any \n -> \r\n translation it needs.
 */
int (*klausscpu_console_out_hook)(int c);

/*
 * Returns the character as an unsigned char, like fputc().  This is also the
 * minimal-libc stdout hook (below), and both printk's and printf's cbprintf
 * abort the rest of the format on a negative return.  char is signed on
 * KlaussCPU, so cbprintf hands a byte >= 0x80 of a "%s" argument or of the
 * format (UTF-8 text, e.g. "free\u2192empty") over as a negative int:
 * returning it unchanged cut the output off after that byte.
 */
int arch_printk_char_out(int c)
{
    int (*hook)(int) = klausscpu_console_out_hook;

    if (hook != NULL && hook(c) != 0) {
        return (unsigned char)c;
    }

    if (c == '\n') {
        while (_UART_STATUS & _UART_TX_BUSY) {
        }
        _UART_TX = '\r';
    }
    while (_UART_STATUS & _UART_TX_BUSY) {
    }
    _UART_TX = (uint32_t)(unsigned char)c;
    return (unsigned char)c;
}

/*
 * Route minimal-libc stdout (used by printf/fprintf/puts when CONFIG_MINIMAL_LIBC=y
 * and CONFIG_STDOUT_CONSOLE=y) through the same UART path as printk.
 *
 * Zephyr's drivers/console/uart_console.c normally calls __stdout_hook_install()
 * at SYS_INIT, but that driver isn't being compiled in this build (no
 * matching CONSOLE_SUBSYS Kconfig), so without this hook the libc default
 * _stdout_hook returns EOF for every byte and printf produces no output.
 */
static int _klausscpu_stdout_install(void)
{
    __stdout_hook_install(arch_printk_char_out);
    return 0;
}

SYS_INIT(_klausscpu_stdout_install, PRE_KERNEL_1, 60);

static int _klausscpu_sched_ready_init(void)
{
    _klausscpu_sched_ready = 1;
    return 0;
}
SYS_INIT(_klausscpu_sched_ready_init, APPLICATION, 99);

/* sys_rand_get() comes from Zephyr's entropy-device generator on the TRNG
 * driver (drivers/entropy, chosen zephyr,entropy), which checks HEALTH_OK. */

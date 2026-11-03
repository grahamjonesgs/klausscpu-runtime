#ifndef ZEPHYR_ARCH_KLAUSSCPU_INCLUDE_KLAUSSCPU_ISR_H_
#define ZEPHYR_ARCH_KLAUSSCPU_INCLUDE_KLAUSSCPU_ISR_H_

/*
 * The tail every interrupt handler (the timer's sys_clock_isr() and a device
 * source's z_klausscpu_irq_dispatch()) ends with: pick the thread to resume.
 * Zephyr's z_reschedule() defers the switch while arch_is_in_isr() is true, so
 * it is done here, at ISR exit.  The ISR stubs in swap.S have saved the
 * interrupted thread's context on its own stack and left that stack pointer in
 * _klausscpu_isr_handle.  If another thread should run, arch_switch() takes
 * the ISR fast path and never returns here.
 *
 * Inline so the timer's tick path is not slower than when it had this code
 * itself.
 */

#include <zephyr/kernel.h>

extern uint64_t _klausscpu_isr_handle;
extern volatile uint32_t _klausscpu_sched_ready;
extern void *z_get_next_switch_handle(void *interrupted);
extern void z_klausscpu_switch(void *switch_to, void **switched_from);

static ALWAYS_INLINE void z_klausscpu_isr_exit(void)
{
	/* Before the first thread exists (early boot, PRE_KERNEL_2) there is no
	 * ready queue: z_get_next_switch_handle() would dereference NULL. */
	if (!_klausscpu_sched_ready) {
		return;
	}

	/* z_get_next_switch_handle(interrupted):
	 *   - writes 'interrupted' to _current->switch_handle
	 *   - picks the highest-priority ready thread
	 *   - if different from current, updates _current and returns new handle
	 *   - if same, returns 'interrupted' unchanged */
	struct k_thread *old = _current;
	void *old_handle = (void *)_klausscpu_isr_handle;
	void *new_handle = z_get_next_switch_handle(old_handle);

	if (new_handle != old_handle) {
		z_klausscpu_switch(new_handle, &old->switch_handle);
	}
}

#endif /* ZEPHYR_ARCH_KLAUSSCPU_INCLUDE_KLAUSSCPU_ISR_H_ */

/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_KLAUSSCPU_STACKTRACE_H
#define _ASM_KLAUSSCPU_STACKTRACE_H

#include <linux/sched.h>
#include <asm/current.h>
#include <asm/thread_info.h>

static __always_inline bool on_thread_stack(void)
{
	return !(((unsigned long)current->stack ^ current_stack_pointer) & ~(THREAD_SIZE - 1));
}

#endif

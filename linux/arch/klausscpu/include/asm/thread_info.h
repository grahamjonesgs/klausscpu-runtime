/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_KLAUSSCPU_THREAD_INFO_H
#define _ASM_KLAUSSCPU_THREAD_INFO_H

#include <asm/page.h>

/* 16 KiB kernel stacks from the slab (in kseg, as frame pushes need). */
#define THREAD_SIZE		16384
#define THREAD_SIZE_ORDER	0
#define THREAD_ALIGN		THREAD_SIZE

#ifndef __ASSEMBLER__

struct thread_info {
	unsigned long	flags;		/* TIF_* */
	int		preempt_count;
	unsigned long	syscall_work;	/* SYSCALL_WORK_* (GENERIC_ENTRY) */
};

#define INIT_THREAD_INFO(tsk)			\
{						\
	.flags		= 0,			\
	.preempt_count	= INIT_PREEMPT_COUNT,	\
}

#endif /* !__ASSEMBLER__ */

#define HAVE_TIF_RESTORE_SIGMASK
#define HAVE_TIF_POLLING_NRFLAG
#include <asm-generic/thread_info_tif.h>

#endif

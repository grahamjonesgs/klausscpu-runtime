/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_KLAUSSCPU_MMU_H
#define _ASM_KLAUSSCPU_MMU_H

typedef struct {
	unsigned long asid;	/* 0 until ASIDs are used: every switch_mm flushes */
} mm_context_t;

#endif

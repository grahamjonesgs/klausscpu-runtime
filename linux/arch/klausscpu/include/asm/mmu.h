/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_KLAUSSCPU_MMU_H
#define _ASM_KLAUSSCPU_MMU_H

typedef struct {
	unsigned long asid;	/* generation << 8 | ASID (asm/mmu_context.h) */
	void *pte_frag;		/* the next free PTE table in this mm's page (mm/pgalloc.c) */
} mm_context_t;

#endif

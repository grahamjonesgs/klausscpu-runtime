/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_KLAUSSCPU_SETUP_H
#define _ASM_KLAUSSCPU_SETUP_H

#include <uapi/asm-generic/setup.h>

#ifndef __ASSEMBLER__
struct pt_regs;
struct task_struct;

/* Arch-internal entry points shared between files. */
void klausscpu_early_console_init(void);
void klausscpu_handle_irq(unsigned int hwirq);
unsigned int klausscpu_irq(unsigned int hwirq);
/* A device on interrupt source 2 (KL_INT2_ETH, KL_INT2_SD): its child IRQ
 * of the cascade, or source 2 itself (Ethernet) on bitstreams without it. */
unsigned int klausscpu_int2_irq(unsigned int child);
extern unsigned long klausscpu_cpu_features;
#ifdef CONFIG_KLAUSSCPU_SDNATIVE
/* The native SD controller is present and in use: the SPI driver stays off. */
bool klausscpu_sd_use_native(void);
#else
static inline bool klausscpu_sd_use_native(void) { return false; }
#endif
void do_page_fault(struct pt_regs *regs, unsigned long cause, unsigned long addr);
/* unaligned.c: misaligned user loads/stores, emulated unless false. */
bool klausscpu_misaligned_user(struct pt_regs *regs, unsigned long cause);
void klausscpu_misaligned_init(void);
unsigned long klausscpu_misaligned_count(void);	/* emulated since boot */
/* mm/tlbwalk.c: the hardware TLB walker, when CPU_FEATURES[25] says so. */
void klausscpu_tlbwalk_init(void);
unsigned long klausscpu_tlb_walks(void);	/* walker refills since boot */
asmlinkage void klausscpu_ret_from_fork(struct task_struct *prev, struct pt_regs *regs,
					int (*fn)(void *), void *arg);
#endif

#endif

/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_KLAUSSCPU_EXTABLE_H
#define _ASM_KLAUSSCPU_EXTABLE_H

/* Absolute 32-bit addresses (all kernel text is in kseg). */
struct exception_table_entry {
	unsigned int insn, fixup;
};

struct pt_regs;
extern int fixup_exception(struct pt_regs *regs);

#endif

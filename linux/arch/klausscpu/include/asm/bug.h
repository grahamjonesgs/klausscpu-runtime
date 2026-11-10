/* SPDX-License-Identifier: GPL-2.0 */
/*
 * BUG()/WARN() as a TRAP instruction (one word, 0x6C04_0000) with a
 * __bug_table entry; the TRAP handler calls report_bug() and steps over the
 * trap for a WARN.  Entries hold absolute addresses (32-bit kseg addresses
 * in 64-bit fields).
 */
#ifndef _ASM_KLAUSSCPU_BUG_H
#define _ASM_KLAUSSCPU_BUG_H

#include <linux/compiler.h>
#include <linux/types.h>

#define KLAUSSCPU_TRAP_INSN	0x6c040000u

#ifdef CONFIG_GENERIC_BUG

#ifdef CONFIG_DEBUG_BUGVERBOSE
#define __BUG_ENTRY(file, line, flags)		\
	"\t.quad 1b\n"				\
	"\t.quad " file "\n"			\
	"\t.short " line "\n"			\
	"\t.short " flags "\n"
#else
#define __BUG_ENTRY(file, line, flags)		\
	"\t.quad 1b\n"				\
	"\t.short " flags "\n"
#endif

#define __BUG_FLAGS(cond_str, flags)					\
do {									\
	__asm__ __volatile__ (						\
		"1:\ttrap\n"						\
		".pushsection __bug_table, \"aw\"\n"			\
		"2:\n"							\
		__BUG_ENTRY("%c0", "%c1", "%c2")			\
		"\t.org 2b + %c3\n"					\
		".popsection"						\
		:							\
		: "i" (WARN_CONDITION_STR(cond_str) __FILE__), "i" (__LINE__), \
		  "i" (flags), "i" (sizeof(struct bug_entry)));		\
} while (0)

#else
#define __BUG_FLAGS(cond_str, flags)	__asm__ __volatile__ ("trap")
#endif

#define BUG() do {						\
	__BUG_FLAGS("", 0);					\
	unreachable();						\
} while (0)
#define __WARN_FLAGS(cond_str, flags)	__BUG_FLAGS(cond_str, BUGFLAG_WARNING | (flags))
#define ARCH_WARN_REACHABLE
#define HAVE_ARCH_BUG

#include <asm-generic/bug.h>

#endif

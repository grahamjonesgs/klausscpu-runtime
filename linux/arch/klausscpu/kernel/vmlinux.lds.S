/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The klausscc loader puts the first LOAD segment at PA 0x20 (after its
 * heap header) and everything else at the same offset from it, so the
 * kernel is linked at kseg 0x8000_0020: VA - 0x8000_0000 = PA.
 */
#define LOAD_OFFSET	PAGE_OFFSET

#include <asm/page.h>
#include <asm/thread_info.h>
#include <asm-generic/vmlinux.lds.h>

ENTRY(_start)
jiffies = jiffies_64;

SECTIONS
{
	. = PAGE_OFFSET + 0x20;
	_text = .;
	.head.text : { KEEP(*(.head.text)) }
	.text : {
		_stext = .;
		TEXT_TEXT
		SCHED_TEXT
		LOCK_TEXT
		KPROBES_TEXT
		IRQENTRY_TEXT
		SOFTIRQENTRY_TEXT
		ENTRY_TEXT
		*(.fixup)
		_etext = .;
	}
	. = ALIGN(PAGE_SIZE);
	_sdata = .;
	RO_DATA(PAGE_SIZE)
	EXCEPTION_TABLE(4)
	RW_DATA(32, PAGE_SIZE, THREAD_SIZE)
	_edata = .;
	. = ALIGN(PAGE_SIZE);
	__init_begin = .;
	INIT_TEXT_SECTION(PAGE_SIZE)
	INIT_DATA_SECTION(16)
	/* Kept, not discarded: __bug_table entries can point into exit code. */
	.exit.text : { EXIT_TEXT }
	.exit.data : { EXIT_DATA }
	PERCPU_SECTION(32)
	. = ALIGN(PAGE_SIZE);
	__init_end = .;
	BSS_SECTION(0, 0, 32)
	_end = .;

	STABS_DEBUG
	DWARF_DEBUG
	ELF_DETAILS
	DISCARDS
}

/* SPDX-License-Identifier: GPL-2.0 */
/*
 * User binaries are ELF32 (klausscpu-llvm: 32-bit addresses) although the
 * ABI is LP64; include/linux/elf.h follows ELF_CLASS.
 */
#ifndef _ASM_KLAUSSCPU_ELF_H
#define _ASM_KLAUSSCPU_ELF_H

#include <uapi/linux/elf.h>
#include <asm/ptrace.h>
#include <asm/page.h>

#define EM_KLAUSSCPU	0x4B43	/* 'K' << 8 | 'C' (klausscpu-llvm) */

#define ELF_ARCH	EM_KLAUSSCPU
#define ELF_CLASS	ELFCLASS32
#define ELF_DATA	ELFDATA2LSB

#define elf_check_arch(x)	((x)->e_machine == EM_KLAUSSCPU)

typedef unsigned long elf_greg_t;
#define ELF_NGREG	(sizeof(struct user_regs_struct) / sizeof(elf_greg_t))
typedef elf_greg_t elf_gregset_t[ELF_NGREG];
typedef struct { unsigned long unused; } elf_fpregset_t;

#define ELF_EXEC_PAGESIZE	PAGE_SIZE
#define ELF_ET_DYN_BASE		(TASK_SIZE / 3 * 2)
#define ELF_HWCAP		(0)
#define ELF_PLATFORM		(NULL)

#define ELF_PLAT_INIT(_r, load_addr)	do { } while (0)

#define ELF_CORE_COPY_REGS(dest, regs)	klausscpu_elf_core_copy_regs((dest), (regs))
struct pt_regs;
extern void klausscpu_elf_core_copy_regs(elf_gregset_t dest, struct pt_regs *regs);

#endif

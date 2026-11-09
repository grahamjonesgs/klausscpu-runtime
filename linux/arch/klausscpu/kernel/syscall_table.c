// SPDX-License-Identifier: GPL-2.0
#include <linux/syscalls.h>
#include <linux/mm.h>
#include <asm/syscall.h>
#include <asm/unistd.h>

SYSCALL_DEFINE6(mmap, unsigned long, addr, unsigned long, len,
		unsigned long, prot, unsigned long, flags,
		unsigned long, fd, unsigned long, off)
{
	if (off & ~PAGE_MASK)
		return -EINVAL;
	return ksys_mmap_pgoff(addr, len, prot, flags, fd, off >> PAGE_SHIFT);
}

asmlinkage long sys_rt_sigreturn(void);

#undef __SYSCALL
#define __SYSCALL(nr, call)	[nr] = (call),
#define __SYSCALL_WITH_COMPAT(nr, native, compat)	__SYSCALL(nr, native)

void *sys_call_table[NR_syscalls] = {
	[0 ... NR_syscalls - 1] = sys_ni_syscall,
#include <asm/syscall_table_64.h>
};

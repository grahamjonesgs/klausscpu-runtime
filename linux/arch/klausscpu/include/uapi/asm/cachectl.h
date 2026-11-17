/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_ASM_KLAUSSCPU_CACHECTL_H
#define _UAPI_ASM_KLAUSSCPU_CACHECTL_H

/*
 * The third argument of cacheflush(2) (syscall 244, as on csky / nios2):
 * which caches to make coherent with memory for [addr, addr + bytes).
 */
#define ICACHE	(1 << 0)
#define DCACHE	(1 << 1)
#define BCACHE	(ICACHE | DCACHE)

#endif /* _UAPI_ASM_KLAUSSCPU_CACHECTL_H */

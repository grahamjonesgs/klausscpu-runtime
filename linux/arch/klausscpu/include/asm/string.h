/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_KLAUSSCPU_STRING_H
#define _ASM_KLAUSSCPU_STRING_H
/* Doubleword-at-a-time versions (arch/klausscpu/lib/string.c). */
#define __HAVE_ARCH_MEMSET
#define __HAVE_ARCH_MEMCPY
#define __HAVE_ARCH_MEMMOVE
extern void *memset(void *s, int c, __kernel_size_t n);
extern void *memcpy(void *dest, const void *src, __kernel_size_t n);
extern void *memmove(void *dest, const void *src, __kernel_size_t n);
#endif

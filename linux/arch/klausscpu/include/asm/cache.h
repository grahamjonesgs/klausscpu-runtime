/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The D-cache: 64 KiB 2-way, 32-byte lines, write-back with one dirty bit
 * per line, and it snoops no other DDR master.  A buffer a device writes
 * by DMA must not share a line with data the CPU writes meanwhile (the
 * line's writeback would clobber the device's bytes, and the invalidate
 * after the DMA would drop the CPU's), so DMA buffers, and with them every
 * kmalloc() object, are 32-byte aligned (mm/dma-noncoherent.c).
 */
#ifndef _ASM_KLAUSSCPU_CACHE_H
#define _ASM_KLAUSSCPU_CACHE_H

#define L1_CACHE_SHIFT		5
#define L1_CACHE_BYTES		(1 << L1_CACHE_SHIFT)

#define ARCH_DMA_MINALIGN	L1_CACHE_BYTES

#endif

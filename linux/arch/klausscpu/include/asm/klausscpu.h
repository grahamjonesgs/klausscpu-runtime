/* SPDX-License-Identifier: GPL-2.0 */
/*
 * KlaussCPU system registers, MMIO addresses and the MMU instructions
 * (klaussCPU MMU_PLAN.md §6, §11; the runtime's exc.h and mmio.h).
 */
#ifndef _ASM_KLAUSSCPU_KLAUSSCPU_H
#define _ASM_KLAUSSCPU_KLAUSSCPU_H

/* System registers (MFSR/MTSR take them by number or name). */
#define SR_STATUS	0	/* [0] UM, [1] DI, [9] TE, [10] MAL64 */
#define SR_EVEC		1
#define SR_ECAUSE	2
#define SR_EADDR	3
#define SR_USP		4	/* the user SP while in supervisor mode */
#define SR_SCRATCH0	5	/* Linux: current task_struct */
#define SR_SCRATCH1	6
#define SR_TP		7	/* user TLS pointer, context-switched */
#define SR_CYCLE	8	/* 64-bit cycle counter */
#define SR_TLB_HI	0x30	/* [31:16] VPN, [8] G, [7:0] ASID */
#define SR_TLB_LO	0x31	/* [26:16] PFN, [3] X, [2] W, [1] R, [0] V */
#define SR_TLB_INDEX	0x32	/* index, [31] P (TLBP missed) */
#define SR_ASID		0x33
#define SR_TLBVEC	0x34
/* The hardware TLB walker (klaussCPU MMU_PLAN §11.8, CPU_FEATURES[25]). */
#define SR_TLB_PTB	0x35	/* [26:3] PA of the current useg pgd */
#define SR_TLB_WALK	0x36	/* [0] EN, [12:8] RR: the next walker refill's entry */
#define SR_TLB_WCNT	0x37	/* 48-bit count of walker refills */
#define TLB_WALK_EN	0x1

#define STATUS_UM	0x1
#define STATUS_DI	0x2
#define STATUS_TE	0x200
#define STATUS_MAL64	0x400	/* misaligned 64-bit accesses trap (HW_PERF_PLAN §4) */

#define TLB_HI_G	0x100
#define TLB_INDEX_P	0x80000000

/* ECAUSE[7:0] (MMU_PLAN §4.1; the runtime's exc.h). */
#define EXC_ILLEGAL		1	/* EADDR = the instruction word */
#define EXC_PRIVILEGED		2
#define EXC_TRAP		3
#define EXC_SYSCALL		4	/* frame PC = the next instruction */
#define EXC_MISALIGNED_LOAD	5	/* EADDR = the effective address */
#define EXC_MISALIGNED_STORE	6
#define EXC_ACCESS_LOAD		7
#define EXC_ACCESS_STORE	8
#define EXC_ACCESS_FETCH	9
#define EXC_MISALIGNED_FETCH	10
#define EXC_TLB_MISS_LOAD	11	/* EADDR = VA, TLB_HI auto-loaded */
#define EXC_TLB_MISS_STORE	12
#define EXC_TLB_MISS_FETCH	13

/* Frame (MMU_PLAN §3.2). */
#define FRAME_PC_MASK		0xffffffffUL
#define FRAME_FLAGS_SHIFT	32
#define FRAME_FLAGS_MASK	(0x7fUL << FRAME_FLAGS_SHIFT)
#define FRAME_INT_MASK_SHIFT	39
#define FRAME_DI		(1UL << 43)
#define FRAME_PM		(1UL << 44)

/* Address map with TE = 1 (MMU_PLAN §11.2). */
#define KSEG_BASE	0x80000000UL
#define KSEG_END	0x88000000UL
#define KMAP_BASE	0xC0000000UL
#define KMAP_END	0xF0000000UL
#define MMIO_BASE	0xF0000000UL

/* MMIO (runtime mmio.h). */
#define KL_UART_BASE		(MMIO_BASE + 0x00010000UL)
#define KL_UART_TX		0x00
#define KL_UART_RX		0x08
#define KL_UART_STATUS		0x10
#define KL_UART_TX_BUSY		0x1
#define KL_UART_RX_EMPTY	0x2

#ifndef __ASSEMBLY__
void kl_uart_rx_poll(void);	/* hvc_uart.c: from the timer tick */
#endif
#define KL_CACHE_BASE		(MMIO_BASE + 0x00050000UL)
#define KL_CACHE_CTRL		0x00
#define KL_CACHE_FLUSH		0x2	/* write back every dirty line */
#define KL_CACHE_INVALIDATE	0x4	/* write back, then drop every line */
#define KL_CACHE_ICACHE_INV	0x8
#define KL_CACHE_RANGE_CLEAN	0x10	/* the lines of [OP_ADDR, +OP_LEN) */
#define KL_CACHE_RANGE_INV	0x20	/* alone: drop them, no writeback */
#define KL_CACHE_RANGE_IINV	0x40	/* drop them from the I-cache */
#define KL_CACHE_STATUS		0x10
#define KL_CACHE_MNT_BUSY	0x1	/* a walk is pending or running */
#define KL_CACHE_OP_ADDR	0x30	/* range op start (PA), [4:0] read 0 */
#define KL_CACHE_OP_LEN		0x38	/* range op length in bytes ([31:0]), 0 = no-op */
#define KL_INTC_BASE		(MMIO_BASE + 0x000F0000UL)
#define KL_INT_MASK		0x00
#define KL_INT_PEND		0x08
#define KL_INT_VEC(n)		(0x10 + 8 * (n))
#define KL_TIMER_PER		0x30
#define KL_TIMER_CNT		0x38
#define KL_CLOCK_MS		0x40
#define KL_CPU_FEATURES		0x48
/* Source 2's cascade (bitstreams with the native SD controller). */
#define KL_INT2_EN		0x50	/* [0] Ethernet, [1] SD; reset 0b01 */
#define KL_INT2_PEND		0x58	/* raw, before INT2_EN */
#define KL_INT2_ETH		0
#define KL_INT2_SD		1
#define KL_NR_IRQS		4
#define KL_IRQ_TIMER		0

#define CPU_FEAT_EXC		0x1
#define CPU_FEAT_TLB		0x4
#define CPU_FEAT_LRSC		0x8
#define CPU_FEAT_TLB_ENTRIES(f)	(((f) >> 8) & 0xff)
#define CPU_FEAT_SDNATIVE	(1UL << 21)	/* native SD controller, INT2 cascade */
#define CPU_FEAT_MAL64		(1UL << 23)	/* STATUS.MAL64 implemented */
#define CPU_FEAT_TLBWALK	(1UL << 25)	/* the hardware TLB walker (SR_TLB_PTB...) */
#define CPU_FEAT_L2		(1UL << 26)	/* the unified L2 (kernel/l2.c) */
#define CPU_FEAT_DCACHE_RANGE	(1UL << 27)	/* cache range ops (D clean/inv, I inv) */
/* The L2 (klaussCPU branch l2; offsets provisional until its release). */
#define KL_L2_INFO		(KL_CACHE_BASE + 0x20)	/* CACHE_INFO format */
#define KL_L2_CTRL		(KL_CACHE_BASE + 0x28)	/* [0] EN, [1] WBINV, [8] BUSY */
#define KL_L2_CTRL_EN		0x1
#define KL_L2_CTRL_WBINV	0x2
#define KL_L2_CTRL_BUSY		0x100

#ifndef __ASSEMBLER__

#define mfsr(sr) ({							\
	unsigned long __v;						\
	asm volatile("mfsr %0, %1" : "=r"(__v) : "i"(sr) : "memory");	\
	__v; })

#define mtsr(sr, val)							\
	asm volatile("mtsr %0, %1" : : "i"(sr), "r"((unsigned long)(val)) : "memory")

static inline void tlb_write_indexed(void) { asm volatile("tlbwi" ::: "memory"); }
static inline void tlb_read(void) { asm volatile("tlbr" ::: "memory"); }
static inline void tlb_probe(void) { asm volatile("tlbp" ::: "memory"); }
static inline void tlb_invalidate_all(void) { asm volatile("tlbinv" ::: "memory"); }

static inline void kl_icache_invalidate(void)
{
	*(volatile unsigned int *)KL_CACHE_BASE = KL_CACHE_ICACHE_INV;
}

#endif /* !__ASSEMBLER__ */
#endif /* _ASM_KLAUSSCPU_KLAUSSCPU_H */

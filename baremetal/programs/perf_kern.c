// perf_kern.c — hand-written instruction-sequence costs for the compiler's
// cost models (schedule model, fetch layout, branch heuristics).
//
// Each kernel repeats one body 16 times inside a counted loop; the cycles of
// the empty loop are subtracted, so a line gives the cycles of one body:
//   KERN,<name>,<cycles x 100 per body>
// The data buffer is in the D-cache (warmed by a first run of each kernel),
// so memory kernels measure hit costs.  Registers: r12 = the buffer, r1 = 1,
// r2 = an alternating 0/1, r9 = the loop counter; bodies use r3-r8, r10, r11.

#include <stdint.h>
#include <stdio.h>
#include "../../mmio.h"

typedef uint64_t u64;

#define ITERS 2000u
#define U(x) ((unsigned long)(x))

static u64 g_buf[512] __attribute__((aligned(64)));

// Call chains for the return-address stack: depthN calls depth(N-1).
__asm__(".text\n"
        ".p2align 4\n"
        "kd1: ret\n"
        "kd2: callrel kd1\n ret\n"
        "kd3: callrel kd2\n ret\n"
        "kd4: callrel kd3\n ret\n"
        "kd5: callrel kd4\n ret\n"
        "kd6: callrel kd5\n ret\n"
        "kd8: callrel kd7\n ret\n"
        "kd7: callrel kd6\n ret\n");

#define KERN(fn, body)                                                       \
  __attribute__((noinline)) static void fn(u64 *buf) {                       \
    __asm__ volatile("addi r12, %0, 0\n"                                     \
                     "setr r1, 1\n"                                          \
                     "setr r2, 0\n"                                          \
                     "setr r9, %1\n"                                         \
                     ".p2align 4\n"                                          \
                     "1:\n"                                                  \
                     ".rept 16\n" body "\n.endr\n"                           \
                     "xori r2, r2, 1\n"                                      \
                     "decr r9, r9\n"                                         \
                     "fbri r9, 0, 8, 1b\n"                                   \
                     :                                                       \
                     : "r"(buf), "i"(ITERS)                                  \
                     : "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", \
                       "r10", "r11", "r12", "r13", "r14", "memory");         \
  }

// The loop alone.
KERN(k_empty, "")

// ALU and fetch.
KERN(k_alu, "addr r4, r5, r6")
KERN(k_alu_dep, "addr r4, r4, r6")
KERN(k_setr2, "setr r4, 0x12345")
KERN(k_alu_setr, "addr r4, r5, r6\n setr r7, 0x12345")
KERN(k_alu_stl, "addr r4, r5, r6\n stidx64 r5, r12, 2048")
KERN(k_alu_ldl, "addr r4, r5, r6\n ldidx64 r3, r12, 2048")

// Loads.
KERN(k_ld, "ldidx64 r3, r12, 8")
KERN(k_ld_alu, "ldidx64 r3, r12, 8\n addr r4, r5, r6")
KERN(k_ld_alu2, "ldidx64 r3, r12, 8\n addr r4, r5, r6\n addr r7, r5, r6")
KERN(k_ld_use, "ldidx64 r3, r12, 8\n addr r4, r3, r6")
KERN(k_ld_alu_use, "ldidx64 r3, r12, 8\n addr r7, r5, r6\n addr r4, r3, r6")
KERN(k_ld_alu2_use,
     "ldidx64 r3, r12, 8\n addr r7, r5, r6\n addr r8, r5, r6\n addr r4, r3, r6")
KERN(k_ld_chain, "ldidx64 r12, r12, 0")
KERN(k_ld32, "ldidx32 r3, r12, 8")
KERN(k_ld8, "ldidx8 r3, r12, 8")
KERN(k_ld_long, "ldidx64 r3, r12, 2048")
KERN(k_ld_long_alu, "ldidx64 r3, r12, 2048\n addr r4, r5, r6")
KERN(k_memget, "memget64 r3, r12")
KERN(k_ldr, "ldidx64r r3, r12, r1")

// Stores.
KERN(k_st, "stidx64 r5, r12, 0")
KERN(k_st_alu, "stidx64 r5, r12, 0\n addr r4, r5, r6")
KERN(k_st_alu2, "stidx64 r5, r12, 0\n addr r4, r5, r6\n addr r7, r5, r6")
KERN(k_st_alu3,
     "stidx64 r5, r12, 0\n addr r4, r5, r6\n addr r7, r5, r6\n addr r8, r5, r6")
KERN(k_st32, "stidx32 r5, r12, 0")
KERN(k_st_long, "stidx64 r5, r12, 2048")

// Mixed memory pairs.
KERN(k_st_ld, "stidx64 r5, r12, 0\n ldidx64 r3, r12, 8")
KERN(k_st_alu_ld, "stidx64 r5, r12, 0\n addr r4, r5, r6\n ldidx64 r3, r12, 8")
KERN(k_st_alu2_ld,
     "stidx64 r5, r12, 0\n addr r4, r5, r6\n addr r7, r5, r6\n ldidx64 r3, r12, 8")
KERN(k_ld_st, "ldidx64 r3, r12, 8\n stidx64 r5, r12, 0")
KERN(k_ld_alu_st, "ldidx64 r3, r12, 8\n addr r4, r5, r6\n stidx64 r5, r12, 0")
KERN(k_ld_st_dep, "ldidx64 r3, r12, 8\n stidx64 r3, r12, 64")
KERN(k_ld_alu_st_dep, "ldidx64 r3, r12, 8\n addr r4, r5, r6\n stidx64 r3, r12, 64")
KERN(k_ld_alu2_st_dep,
     "ldidx64 r3, r12, 8\n addr r4, r5, r6\n addr r7, r5, r6\n stidx64 r3, r12, 64")

// 32-byte copies (one body = 4 doublewords).
KERN(k_cp_4l4s,
     "ldidx64 r3, r12, 0\n ldidx64 r4, r12, 8\n ldidx64 r5, r12, 16\n"
     " ldidx64 r6, r12, 24\n stidx64 r3, r12, 64\n stidx64 r4, r12, 72\n"
     " stidx64 r5, r12, 80\n stidx64 r6, r12, 88")
KERN(k_cp_2l2s,
     "ldidx64 r3, r12, 0\n ldidx64 r4, r12, 8\n stidx64 r3, r12, 64\n"
     " stidx64 r4, r12, 72\n ldidx64 r5, r12, 16\n ldidx64 r6, r12, 24\n"
     " stidx64 r5, r12, 80\n stidx64 r6, r12, 88")
KERN(k_cp_pipe,
     "ldidx64 r3, r12, 0\n ldidx64 r4, r12, 8\n stidx64 r3, r12, 64\n"
     " ldidx64 r5, r12, 16\n stidx64 r4, r12, 72\n ldidx64 r6, r12, 24\n"
     " stidx64 r5, r12, 80\n stidx64 r6, r12, 88")
KERN(k_cp_ls,
     "ldidx64 r3, r12, 0\n stidx64 r3, r12, 64\n ldidx64 r4, r12, 8\n"
     " stidx64 r4, r12, 72\n ldidx64 r5, r12, 16\n stidx64 r5, r12, 80\n"
     " ldidx64 r6, r12, 24\n stidx64 r6, r12, 88")
// The same copy with the pointer bumps of a loop between the accesses.
KERN(k_cp_4l4s_alu,
     "ldidx64 r3, r12, 0\n ldidx64 r4, r12, 8\n addr r10, r10, r1\n"
     " ldidx64 r5, r12, 16\n ldidx64 r6, r12, 24\n stidx64 r3, r12, 64\n"
     " stidx64 r4, r12, 72\n addr r11, r11, r1\n stidx64 r5, r12, 80\n"
     " stidx64 r6, r12, 88")
KERN(k_cp_ilv_alu,
     "ldidx64 r3, r12, 0\n addr r10, r10, r1\n ldidx64 r4, r12, 8\n"
     " stidx64 r3, r12, 64\n addr r11, r11, r1\n ldidx64 r5, r12, 16\n"
     " stidx64 r4, r12, 72\n ldidx64 r6, r12, 24\n stidx64 r5, r12, 80\n"
     " stidx64 r6, r12, 88")

// Stack.
KERN(k_push_pop, "push r5\n pop r3")
KERN(k_push_alu_pop, "push r5\n addr r4, r5, r6\n pop r3")

// Branches.  r1 = 1, so NE against 0 is taken and EQ is not; r2 alternates
// between loop iterations, so a branch on it is never predicted taken.
KERN(k_fbr_nt, "fbri r1, 0, 0, 2f\n 2:")
KERN(k_fbr_t, "fbri r1, 0, 8, 2f\n 2:")
KERN(k_fbr_t_alu, "fbri r1, 0, 8, 2f\n 2: addr r4, r5, r6")
KERN(k_fbr_alt, "fbri r2, 0, 8, 2f\n 2:")
KERN(k_fbrv_t, "fbrv r1, 1000, 8, 2f\n 2:")
KERN(k_cmp_jne_t, "cmprv r1, 0\n jmpne 2f\n 2:")
KERN(k_cmp_jne_nt, "cmprv r1, 1\n jmpne 2f\n 2:")
KERN(k_cmp_alu_jne_t, "cmprv r1, 0\n addr r4, r5, r6\n jmpne 2f\n 2:")
KERN(k_jmp, "jmprel 2f\n 2:")
KERN(k_jmp_fwd8, "jmprel 2f\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n 2:")
KERN(k_leapc, "leapc r3, 2f\n 2:")
KERN(k_jmpr, "leapc r3, 2f\n jmpr r3\n 2:")
KERN(k_ld_fbr_t, "ldidx64 r3, r12, 8\n fbri r1, 0, 8, 2f\n 2:")
KERN(k_st_fbr_t, "stidx64 r5, r12, 0\n fbri r1, 0, 8, 2f\n 2:")
KERN(k_ld_use_fbr, "ldidx64 r3, r12, 8\n fbri r3, 0, 8, 2f\n 2:")
KERN(k_mul, "mulr r4, r5, r6")
KERN(k_mul_alu, "mulr r4, r5, r6\n addr r7, r5, r6")

// Calls: callrel to a ret (CALL + RET), and chains 2..8 deep.
KERN(k_call1, "callrel kd1")
KERN(k_call2, "callrel kd2")
KERN(k_call3, "callrel kd3")
KERN(k_call4, "callrel kd4")
KERN(k_call5, "callrel kd5")
KERN(k_call6, "callrel kd6")
KERN(k_call8, "callrel kd8")


// Loop kernels: an inner loop of 64 iterations whose head sits PAD words
// into an aligned 16-byte line (the PAD NOPs run once per entry), so a line
// gives the cycles of one iteration, branch included, at that phase:
//   LOOP,<name>,<pad>,<cycles x 100 per iteration>
#define LOOPK(fn, pad, body)                                                 \
  __attribute__((noinline)) static void fn(u64 *buf) {                       \
    __asm__ volatile("addi r12, %0, 0\n"                                     \
                     "setr r1, 1\n"                                          \
                     "setr r9, %1\n"                                         \
                     "1:\n"                                                  \
                     "setr r11, 64\n"                                        \
                     ".p2align 4\n"                                          \
                     ".rept " #pad "\n nop\n .endr\n"                        \
                     "3:\n" body "\n"                                        \
                     "decr r11, r11\n"                                       \
                     "fbri r11, 0, 8, 3b\n"                                  \
                     "decr r9, r9\n"                                         \
                     "fbri r9, 0, 8, 1b\n"                                   \
                     :                                                       \
                     : "r"(buf), "i"(ITERS)                                  \
                     : "r1", "r3", "r4", "r5", "r6", "r7", "r8", "r9",       \
                       "r11", "r12", "memory");                              \
  }
// The same with the decrement written by the body (no decrement in front
// of the branch).
#define LOOPE(fn, pad, body)                                                 \
  __attribute__((noinline)) static void fn(u64 *buf) {                       \
    __asm__ volatile("addi r12, %0, 0\n"                                     \
                     "setr r1, 1\n"                                          \
                     "setr r9, %1\n"                                         \
                     "1:\n"                                                  \
                     "setr r11, 64\n"                                        \
                     ".p2align 4\n"                                          \
                     ".rept " #pad "\n nop\n .endr\n"                        \
                     "3:\n" body "\n"                                        \
                     "fbri r11, 0, 8, 3b\n"                                  \
                     "decr r9, r9\n"                                         \
                     "fbri r9, 0, 8, 1b\n"                                   \
                     :                                                       \
                     : "r"(buf), "i"(ITERS)                                  \
                     : "r1", "r3", "r4", "r5", "r6", "r7", "r8", "r9",       \
                       "r11", "r12", "memory");                              \
  }
#define LOOPE4(name, body)                                                   \
  LOOPE(l_##name##_0, 0, body) LOOPE(l_##name##_1, 1, body)                  \
  LOOPE(l_##name##_2, 2, body) LOOPE(l_##name##_3, 3, body)
#define LOOP4(name, body)                                                    \
  LOOPK(l_##name##_0, 0, body) LOOPK(l_##name##_1, 1, body)                  \
  LOOPK(l_##name##_2, 2, body) LOOPK(l_##name##_3, 3, body)

LOOP4(empty, "")
LOOP4(alu1, "addr r4, r5, r6")
LOOP4(alu4, "addr r4, r5, r6\n addr r7, r5, r6\n addr r8, r5, r6\n addr r3, r5, r6")
LOOP4(setr_first, "setr r4, 0x12345\n addr r7, r5, r6\n addr r8, r5, r6")
LOOP4(setr_mid, "addr r7, r5, r6\n setr r4, 0x12345\n addr r8, r5, r6")
LOOP4(stl_first, "stidx64 r5, r12, 2048\n addr r7, r5, r6\n addr r8, r5, r6")
LOOP4(stl_mid, "addr r7, r5, r6\n stidx64 r5, r12, 2048\n addr r8, r5, r6")
LOOP4(ld_alu2, "ldidx64 r3, r12, 8\n addr r7, r5, r6\n addr r8, r5, r6")
LOOP4(alu8, "addr r4, r5, r6\n addr r7, r5, r6\n addr r8, r5, r6\n addr r3, r5, r6\n"
            " addr r4, r5, r6\n addr r7, r5, r6\n addr r8, r5, r6\n addr r3, r5, r6")

LOOPE4(e_alu0, "decr r11, r11")
LOOPE4(e_alu1, "decr r11, r11\n addr r4, r5, r6")
LOOPE4(e_alu2, "decr r11, r11\n addr r4, r5, r6\n addr r7, r5, r6")
LOOPE4(e_alu4, "decr r11, r11\n addr r4, r5, r6\n addr r7, r5, r6\n addr r8, r5, r6\n addr r3, r5, r6")
LOOPE4(e_cmp_jne, "decr r11, r11\n addr r4, r5, r6\n addr r7, r5, r6\n cmprv r11, 0\n jmpne 3b\n jmprel 5f\n 5:")
LOOPE4(e_jmp_back, "decr r11, r11\n addr r4, r5, r6\n addr r7, r5, r6\n fbri r11, 0, 0, 5f\n jmprel 3b\n 5:")

// matmult-int's -Os inner loop in the old (two loads back to back) and the
// new (an ALU op between them) post-RA order.
LOOPE4(mm_old, "ldidx64 r4, r12, 8\n ldidx64 r5, r12, 16\n decr r11, r11\n addv r8, r8, 160\n mulr r4, r5, r4\n addr r3, r3, r4\n stidx64 r3, r12, 32")
LOOPE4(mm_new, "ldidx64 r4, r12, 8\n decr r11, r11\n ldidx64 r5, r12, 16\n addv r8, r8, 160\n mulr r4, r5, r4\n addr r3, r3, r4\n stidx64 r3, r12, 32")

// One instruction K + 1 words into the loop (K ALU ops behind the decrement
// at the aligned head), ALU ops to fill 12 instructions.
LOOPE(l_xs0, 0, "decr r11, r11\n setr r7, 0x12345\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_xs1, 0, "decr r11, r11\n addr r4, r5, r6\n setr r7, 0x12345\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_xs2, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n setr r7, 0x12345\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_xs3, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n setr r7, 0x12345\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_xs4, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n setr r7, 0x12345\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_xs5, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n setr r7, 0x12345\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_xs6, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n setr r7, 0x12345\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_xs7, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n setr r7, 0x12345\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_xs8, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n setr r7, 0x12345\n addr r4, r5, r6\n ")
LOOPE(l_xs9, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n setr r7, 0x12345\n ")
LOOPE(l_xst0, 0, "decr r11, r11\n stidx64 r5, r12, 2048\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_xst1, 0, "decr r11, r11\n addr r4, r5, r6\n stidx64 r5, r12, 2048\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_xst2, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n stidx64 r5, r12, 2048\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_xst3, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n stidx64 r5, r12, 2048\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_xst4, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n stidx64 r5, r12, 2048\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_xst5, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n stidx64 r5, r12, 2048\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_xst6, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n stidx64 r5, r12, 2048\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_xst7, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n stidx64 r5, r12, 2048\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_xst8, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n stidx64 r5, r12, 2048\n addr r4, r5, r6\n ")
LOOPE(l_xst9, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n stidx64 r5, r12, 2048\n ")
LOOPE(l_xld0, 0, "decr r11, r11\n ldidx64 r3, r12, 2048\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_xld1, 0, "decr r11, r11\n addr r4, r5, r6\n ldidx64 r3, r12, 2048\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_xld2, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n ldidx64 r3, r12, 2048\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_xld3, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ldidx64 r3, r12, 2048\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_xld4, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ldidx64 r3, r12, 2048\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_xld5, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ldidx64 r3, r12, 2048\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_xld6, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ldidx64 r3, r12, 2048\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_xld7, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ldidx64 r3, r12, 2048\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_xld8, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ldidx64 r3, r12, 2048\n addr r4, r5, r6\n ")
LOOPE(l_xld9, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ldidx64 r3, r12, 2048\n ")
LOOPE(l_x1st0, 0, "decr r11, r11\n stidx64 r5, r12, 0\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_x1st1, 0, "decr r11, r11\n addr r4, r5, r6\n stidx64 r5, r12, 0\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_x1st2, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n stidx64 r5, r12, 0\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_x1st3, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n stidx64 r5, r12, 0\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_x1st4, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n stidx64 r5, r12, 0\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_x1st5, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n stidx64 r5, r12, 0\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_x1st6, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n stidx64 r5, r12, 0\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_x1st7, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n stidx64 r5, r12, 0\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_x1st8, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n stidx64 r5, r12, 0\n addr r4, r5, r6\n ")
LOOPE(l_x1st9, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n stidx64 r5, r12, 0\n ")
LOOPE(l_x1ld0, 0, "decr r11, r11\n ldidx64 r3, r12, 8\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_x1ld1, 0, "decr r11, r11\n addr r4, r5, r6\n ldidx64 r3, r12, 8\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_x1ld2, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n ldidx64 r3, r12, 8\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_x1ld3, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ldidx64 r3, r12, 8\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_x1ld4, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ldidx64 r3, r12, 8\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_x1ld5, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ldidx64 r3, r12, 8\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_x1ld6, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ldidx64 r3, r12, 8\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_x1ld7, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ldidx64 r3, r12, 8\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_x1ld8, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ldidx64 r3, r12, 8\n addr r4, r5, r6\n ")
LOOPE(l_x1ld9, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ldidx64 r3, r12, 8\n ")
LOOPE(l_xs30, 0, "decr r11, r11\n setr64 r7, 1, 2\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_xs31, 0, "decr r11, r11\n addr r4, r5, r6\n setr64 r7, 1, 2\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_xs32, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n setr64 r7, 1, 2\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_xs33, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n setr64 r7, 1, 2\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_xs34, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n setr64 r7, 1, 2\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_xs35, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n setr64 r7, 1, 2\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_xs36, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n setr64 r7, 1, 2\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_xs37, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n setr64 r7, 1, 2\n addr r4, r5, r6\n addr r4, r5, r6\n ")
LOOPE(l_xs38, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n setr64 r7, 1, 2\n addr r4, r5, r6\n ")
LOOPE(l_xs39, 0, "decr r11, r11\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n addr r4, r5, r6\n setr64 r7, 1, 2\n ")

typedef struct { const char *name; int pad; void (*fn)(u64 *); } loop_t;
#define L4(n) {#n, 0, l_##n##_0}, {#n, 1, l_##n##_1}, {#n, 2, l_##n##_2}, {#n, 3, l_##n##_3}
static const loop_t LOOPS[] = {
  L4(empty), L4(alu1), L4(alu4), L4(setr_first), L4(setr_mid), L4(stl_first),
  L4(stl_mid), L4(ld_alu2), L4(alu8),
  L4(e_alu0), L4(e_alu1), L4(e_alu2), L4(e_alu4), L4(e_cmp_jne), L4(e_jmp_back), L4(mm_old), L4(mm_new),
  {"xs_at", 0, l_xs0}, {"xs_at", 1, l_xs1}, {"xs_at", 2, l_xs2}, {"xs_at", 3, l_xs3}, {"xs_at", 4, l_xs4}, {"xs_at", 5, l_xs5}, {"xs_at", 6, l_xs6}, {"xs_at", 7, l_xs7}, {"xs_at", 8, l_xs8}, {"xs_at", 9, l_xs9}, {"xst_at", 0, l_xst0}, {"xst_at", 1, l_xst1}, {"xst_at", 2, l_xst2}, {"xst_at", 3, l_xst3}, {"xst_at", 4, l_xst4}, {"xst_at", 5, l_xst5}, {"xst_at", 6, l_xst6}, {"xst_at", 7, l_xst7}, {"xst_at", 8, l_xst8}, {"xst_at", 9, l_xst9}, {"xld_at", 0, l_xld0}, {"xld_at", 1, l_xld1}, {"xld_at", 2, l_xld2}, {"xld_at", 3, l_xld3}, {"xld_at", 4, l_xld4}, {"xld_at", 5, l_xld5}, {"xld_at", 6, l_xld6}, {"xld_at", 7, l_xld7}, {"xld_at", 8, l_xld8}, {"xld_at", 9, l_xld9}, {"x1st_at", 0, l_x1st0}, {"x1st_at", 1, l_x1st1}, {"x1st_at", 2, l_x1st2}, {"x1st_at", 3, l_x1st3}, {"x1st_at", 4, l_x1st4}, {"x1st_at", 5, l_x1st5}, {"x1st_at", 6, l_x1st6}, {"x1st_at", 7, l_x1st7}, {"x1st_at", 8, l_x1st8}, {"x1st_at", 9, l_x1st9}, {"x1ld_at", 0, l_x1ld0}, {"x1ld_at", 1, l_x1ld1}, {"x1ld_at", 2, l_x1ld2}, {"x1ld_at", 3, l_x1ld3}, {"x1ld_at", 4, l_x1ld4}, {"x1ld_at", 5, l_x1ld5}, {"x1ld_at", 6, l_x1ld6}, {"x1ld_at", 7, l_x1ld7}, {"x1ld_at", 8, l_x1ld8}, {"x1ld_at", 9, l_x1ld9}, {"xs3_at", 0, l_xs30}, {"xs3_at", 1, l_xs31}, {"xs3_at", 2, l_xs32}, {"xs3_at", 3, l_xs33}, {"xs3_at", 4, l_xs34}, {"xs3_at", 5, l_xs35}, {"xs3_at", 6, l_xs36}, {"xs3_at", 7, l_xs37}, {"xs3_at", 8, l_xs38}, {"xs3_at", 9, l_xs39},
};

typedef struct { const char *name; void (*fn)(u64 *); } kern_t;

#define K(n) {&#n[2], n}
static const kern_t KERNS[] = {
  K(k_alu), K(k_alu_dep), K(k_setr2), K(k_alu_setr), K(k_alu_stl), K(k_alu_ldl),
  K(k_ld), K(k_ld_alu), K(k_ld_alu2), K(k_ld_use), K(k_ld_alu_use),
  K(k_ld_alu2_use), K(k_ld_chain), K(k_ld32), K(k_ld8), K(k_ld_long),
  K(k_ld_long_alu), K(k_memget), K(k_ldr),
  K(k_st), K(k_st_alu), K(k_st_alu2), K(k_st_alu3), K(k_st32), K(k_st_long),
  K(k_st_ld), K(k_st_alu_ld), K(k_st_alu2_ld), K(k_ld_st), K(k_ld_alu_st),
  K(k_ld_st_dep), K(k_ld_alu_st_dep), K(k_ld_alu2_st_dep),
  K(k_cp_4l4s), K(k_cp_2l2s), K(k_cp_pipe), K(k_cp_ls), K(k_cp_4l4s_alu),
  K(k_cp_ilv_alu),
  K(k_push_pop), K(k_push_alu_pop),
  K(k_fbr_nt), K(k_fbr_t), K(k_fbr_t_alu), K(k_fbr_alt), K(k_fbrv_t),
  K(k_cmp_jne_t), K(k_cmp_jne_nt), K(k_cmp_alu_jne_t), K(k_jmp), K(k_jmp_fwd8),
  K(k_leapc), K(k_jmpr), K(k_ld_fbr_t), K(k_st_fbr_t), K(k_ld_use_fbr),
  K(k_mul), K(k_mul_alu),
  K(k_call1), K(k_call2), K(k_call3), K(k_call4), K(k_call5), K(k_call6),
  K(k_call8),
};

static u64 run(void (*fn)(u64 *)) {
  fn(g_buf); // warm the I-cache, the D-cache and the predictor
  // k_ld_chain follows g_buf[0]; keep it pointing at itself.
  g_buf[0] = (u64)(uintptr_t)g_buf;
  u64 t0 = REG_PERF_CYCLES;
  fn(g_buf);
  u64 t1 = REG_PERF_CYCLES;
  return t1 - t0;
}

int main(void) {
  g_buf[0] = (u64)(uintptr_t)g_buf;
  printf("=== perf_kern: cycles x100 per body (%u x 16 bodies) ===\n", ITERS);
  u64 empty = run(k_empty);
  printf("KERN,empty_loop_iter,%lu\n", U(empty * 100 / ITERS));
  for (unsigned i = 0; i < sizeof KERNS / sizeof KERNS[0]; i++) {
    u64 c = run(KERNS[i].fn);
    u64 net = c > empty ? c - empty : 0;
    printf("KERN,%s,%lu\n", KERNS[i].name, U(net * 100 / (ITERS * 16u)));
  }
  for (unsigned i = 0; i < sizeof LOOPS / sizeof LOOPS[0]; i++) {
    u64 c = run(LOOPS[i].fn);
    printf("LOOP,%s,%d,%lu\n", LOOPS[i].name, LOOPS[i].pad,
           U(c * 100 / (ITERS * 64u)));
  }
  printf("=== done ===\n");
  return 0;
}

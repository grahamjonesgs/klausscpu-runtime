/* klausscpu_elf.h — KlaussCPU ELF constants for run-time loaders
 *
 * Mirror of klausscpu-llvm's llvm/include/llvm/BinaryFormat/ELF.h
 * (EM_KLAUSSCPU) and llvm/include/llvm/BinaryFormat/ELFRelocs/KlaussCPU.def
 * (the relocation types), for the loaders that relocate code at run time:
 * baremetal/programs/loader.c and freertos/telnet/pic_loader.c.  The Zephyr
 * arch keeps its own copy (arch/klausscpu/core/elf.c).
 * check-toolchain-constants.py compares all of them with the LLVM sources.
 */
#ifndef KLAUSSCPU_ELF_H
#define KLAUSSCPU_ELF_H

#define EM_KLAUSSCPU 0x4B43u /* 'K' << 8 | 'C' */

/* Relocation types: S = symbol, A = addend, P = the relocated field. */
#define R_KLAUSSCPU_NONE    0u
#define R_KLAUSSCPU_ABS32   1u /* S + A, 32 bits */
#define R_KLAUSSCPU_ABS64   2u /* S + A, 64 bits */
#define R_KLAUSSCPU_PCREL32 3u /* S + A - P + 4: word 1 of an instruction,
                                  relative to the instruction itself */
#define R_KLAUSSCPU_PC32    4u /* S + A - P, 32-bit data */
#define R_KLAUSSCPU_PC64    5u /* S + A - P, 64-bit data */
#define R_KLAUSSCPU_CALL18  6u /* (S + A - P) / 4 in word0[17:0] of a
                                  short CALL (0x63000000) or a tail
                                  call's short jump (0x61000000),
                                  P = the instruction */

/* A PCREL32 field sits in word 1, this far after its instruction. */
#define KLAUSSCPU_PCREL32_BIAS 4

#endif /* KLAUSSCPU_ELF_H */

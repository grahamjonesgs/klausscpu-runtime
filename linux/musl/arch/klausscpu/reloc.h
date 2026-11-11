/*
 * KlaussCPU dynamic linking: klausscpu-llvm ABI.md "Dynamic linking".
 *
 * The files are ELFCLASS32 although the C ABI is LP64: the dynamic
 * section and the Elf32_Rela tables are arrays of 32-bit words (r_addend
 * sign-extended), while every field a relocation writes is 64 bits
 * (DL_ELFCLASS, linux/musl/patches/0001-*).  The numbers are those of
 * klausscpu-llvm's ELFRelocs/KlaussCPU.def (check-toolchain-constants.py
 * compares them).
 */
#define LDSO_ARCH "klausscpu"

#define DL_ELFCLASS 32

#define TPOFF_K 0

#define REL_SYMBOLIC    2	/* R_KLAUSSCPU_ABS64 */
#define REL_COPY        12	/* R_KLAUSSCPU_COPY */
#define REL_GOT         13	/* R_KLAUSSCPU_GLOB_DAT */
#define REL_PLT         14	/* R_KLAUSSCPU_JUMP_SLOT */
#define REL_RELATIVE    15	/* R_KLAUSSCPU_RELATIVE */
#define REL_TPOFF       16	/* R_KLAUSSCPU_TPOFF64 */
#define REL_DTPMOD      17	/* R_KLAUSSCPU_DTPMOD64 */
#define REL_DTPOFF      18	/* R_KLAUSSCPU_DTPOFF64 */

/* map_library refuses another machine, class or C ABI version (ABI.md
 * "Versioning": the low byte of e_flags). */
#ifndef EM_KLAUSSCPU
#define EM_KLAUSSCPU            0x4B43	/* 'K' << 8 | 'C' */
#endif
#define KLAUSSCPU_ABI_VERSION   3
#define KLAUSSCPU_EF_ABI_MASK   0xff
#define ARCH_EHDR_REJECT(eh) ( \
	(eh)->e_ident[EI_CLASS] != ELFCLASS32 || \
	(eh)->e_machine != EM_KLAUSSCPU || \
	((eh)->e_flags & KLAUSSCPU_EF_ABI_MASK) != KLAUSSCPU_ABI_VERSION )

#define CRTJMP(pc,sp) __asm__ __volatile__( \
	"setsp %1\n\tjmpr %0" : : "r"(pc), "r"(sp) : "memory" )

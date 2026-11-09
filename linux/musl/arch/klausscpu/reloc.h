/*
 * Static linking only: no dynamic linker and no PIC for KlaussCPU yet, so
 * there is no RELATIVE relocation.  rcrt1.o (static PIE) is built but
 * unusable; REL_RELATIVE is a type no object carries.
 */
#define LDSO_ARCH "klausscpu"

#define TPOFF_K 0

#define REL_SYMBOLIC    2	/* R_KLAUSSCPU_ABS64 */
#define REL_RELATIVE    255	/* none */

#define CRTJMP(pc,sp) __asm__ __volatile__( \
	"setsp %1\n\tjmpr %0" : : "r"(pc), "r"(sp) : "memory" )

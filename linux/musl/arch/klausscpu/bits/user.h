/* The layout of the kernel's struct user_pt_regs (arch/klausscpu/include/uapi/asm/ptrace.h). */
struct user_regs_struct {
	unsigned long r[16];
	unsigned long sp;
	unsigned long pc;
	unsigned long flags;
};
#define ELF_NGREG 19
typedef unsigned long elf_greg_t, elf_gregset_t[ELF_NGREG];
typedef struct { unsigned long __unused; } elf_fpregset_t;

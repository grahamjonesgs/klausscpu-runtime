typedef uint32_t Elf_Symndx;

/* LP64 in ELFCLASS32 files (klausscpu-llvm ABI.md "<link.h> and the
 * loader's structures"): ElfW(Phdr), ElfW(Dyn), ElfW(Sym), ElfW(Half)
 * and ElfW(Addr) are the Elf32 types, but the address fields of
 * dl_phdr_info, link_map and r_debug hold load addresses in memory and
 * are 8 bytes, like the pointers beside them. */
#define ElfW(type) Elf32_ ## type
#define __ELFW_ADDR unsigned long

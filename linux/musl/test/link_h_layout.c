/* link_h_layout.c — <link.h> against klausscpu-llvm ABI.md "<link.h> and
 * the loader's structures" (LP64 in ELFCLASS32 files).  Compile only:
 *   clang --target=klausscpu-unknown-linux-musl --sysroot=SYSROOT \
 *     -fsyntax-only link_h_layout.c */
#include <link.h>
#include <stddef.h>

#define SAME_TYPE(a, b) _Generic((a), b: 1, default: 0)

_Static_assert(sizeof(ElfW(Phdr)) == sizeof(Elf32_Phdr), "ElfW(Phdr)");
_Static_assert(sizeof(ElfW(Dyn)) == sizeof(Elf32_Dyn), "ElfW(Dyn)");
_Static_assert(sizeof(ElfW(Sym)) == sizeof(Elf32_Sym), "ElfW(Sym)");
_Static_assert(sizeof(ElfW(Addr)) == 4, "ElfW(Addr) is Elf32_Addr");

_Static_assert(offsetof(struct dl_phdr_info, dlpi_addr) == 0, "dlpi_addr");
_Static_assert(sizeof(((struct dl_phdr_info *)0)->dlpi_addr) == 8, "dlpi_addr size");
_Static_assert(offsetof(struct dl_phdr_info, dlpi_name) == 8, "dlpi_name");
_Static_assert(offsetof(struct dl_phdr_info, dlpi_phdr) == 16, "dlpi_phdr");
_Static_assert(SAME_TYPE(((struct dl_phdr_info *)0)->dlpi_phdr, const Elf32_Phdr *), "dlpi_phdr type");
_Static_assert(offsetof(struct dl_phdr_info, dlpi_phnum) == 24, "dlpi_phnum");
_Static_assert(sizeof(((struct dl_phdr_info *)0)->dlpi_phnum) == 2, "dlpi_phnum size");
_Static_assert(offsetof(struct dl_phdr_info, dlpi_adds) == 32, "dlpi_adds");
_Static_assert(offsetof(struct dl_phdr_info, dlpi_subs) == 40, "dlpi_subs");
_Static_assert(offsetof(struct dl_phdr_info, dlpi_tls_modid) == 48, "dlpi_tls_modid");
_Static_assert(offsetof(struct dl_phdr_info, dlpi_tls_data) == 56, "dlpi_tls_data");
_Static_assert(sizeof(struct dl_phdr_info) == 64, "dl_phdr_info size");

_Static_assert(offsetof(struct link_map, l_addr) == 0, "l_addr");
_Static_assert(sizeof(((struct link_map *)0)->l_addr) == 8, "l_addr size");
_Static_assert(offsetof(struct link_map, l_name) == 8, "l_name");
_Static_assert(offsetof(struct link_map, l_ld) == 16, "l_ld");
_Static_assert(SAME_TYPE(((struct link_map *)0)->l_ld, Elf32_Dyn *), "l_ld type");
_Static_assert(offsetof(struct link_map, l_next) == 24, "l_next");
_Static_assert(offsetof(struct link_map, l_prev) == 32, "l_prev");
_Static_assert(sizeof(struct link_map) == 40, "link_map size");

_Static_assert(offsetof(struct r_debug, r_version) == 0, "r_version");
_Static_assert(offsetof(struct r_debug, r_map) == 8, "r_map");
_Static_assert(offsetof(struct r_debug, r_brk) == 16, "r_brk");
_Static_assert(sizeof(((struct r_debug *)0)->r_brk) == 8, "r_brk size");
_Static_assert(offsetof(struct r_debug, r_state) == 24, "r_state");
_Static_assert(offsetof(struct r_debug, r_ldbase) == 32, "r_ldbase");
_Static_assert(sizeof(((struct r_debug *)0)->r_ldbase) == 8, "r_ldbase size");
_Static_assert(sizeof(struct r_debug) == 40, "r_debug size");

#ifdef __ELFW_ADDR
#error "__ELFW_ADDR leaks out of <link.h>"
#endif

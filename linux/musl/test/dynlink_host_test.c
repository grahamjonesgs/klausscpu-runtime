/*
 * dynlink_host_test.c — the patched musl dynamic linker's ELF32-on-LP64
 * code, run on the (LP64, little-endian) host against a real KlaussCPU
 * shared object.  dynlink_host_test.sh builds it: it cuts the code under
 * test out of the patched ldso/dynlink.c (dynlink_parts.h: struct dso,
 * laddr, decode_vec/decode_dynv/search_dynv, the SysV and GNU hash
 * lookups, find_sym2, do_relocs, decode_dyn, count_syms) and compiles it
 * with the patched src/internal/dynlink.h and the port's reloc.h, so
 * Word is Elf32_Word and size_t 64 bits, as on KlaussCPU.
 *
 * Checks, against an independent reading of the file with <elf.h>'s
 * Elf32 structures:
 *  1. decode_dyn finds DT_SYMTAB, DT_STRTAB, DT_HASH, DT_GNU_HASH, and
 *     search_dynv / decode_dynv agree with every Elf32_Dyn entry;
 *  2. count_syms (from .hash, and from .gnu.hash alone) is the symbol
 *     count;
 *  3. every defined dynamic symbol is found by name through the GNU hash
 *     (32-bit Bloom words) and through the SysV hash, and absent names are
 *     not;
 *  4. do_relocs over DT_JMPREL and DT_RELA writes exactly the 64-bit
 *     values the ABI says (RELATIVE B+A, GLOB_DAT/JUMP_SLOT/ABS64 S+A) and
 *     changes no other byte;
 *  5. a synthetic RELA table: negative addends (RELATIVE, ABS64), TPOFF64
 *     (a negative 64-bit TP offset), DTPMOD64, DTPOFF64, COPY, and the
 *     12-byte stride.
 */
#define _GNU_SOURCE
#include <elf.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <pthread.h>

#define hidden
#define weak

/* What the cut-out code needs from musl's internals. */
typedef uint32_t Elf_Symndx;
struct tls_module {
	struct tls_module *next;
	void *image;
	size_t len, size, align, offset;
};
#define DTP_OFFSET 0
#include "dynlink.h"

_Static_assert(sizeof(Word) == 4 && sizeof(Sword) == 4 && DL_ELF_NARROW,
	"the host test needs an LP64 host and DL_ELFCLASS 32");
_Static_assert(sizeof(size_t) == 8, "LP64 host");

static int runtime;
static jmp_buf *rtld_fail;
static size_t static_tls_cnt;
static int errors_seen;
static void error(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("loader error: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	errors_seen++;
}
hidden ptrdiff_t __tlsdesc_static() { return 0; }
hidden ptrdiff_t __tlsdesc_dynamic() { return 0; }

#include "dynlink_parts.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
	fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static unsigned char *load(const char *path, size_t *len, Elf32_Ehdr **ehp)
{
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); exit(2); }
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char *file = malloc(n);
	if (fread(file, 1, n, f) != (size_t)n) { perror("read"); exit(2); }
	fclose(f);
	Elf32_Ehdr *eh = (void *)file;
	if (memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_ident[EI_CLASS] != ELFCLASS32) {
		fprintf(stderr, "%s: not ELF32\n", path); exit(2);
	}
	/* Lay the PT_LOAD segments out at their p_vaddr, as map_library would. */
	size_t end = 0;
	Elf32_Phdr *ph = (void *)(file + eh->e_phoff);
	for (int i = 0; i < eh->e_phnum; i++)
		if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr + ph[i].p_memsz > end)
			end = ph[i].p_vaddr + ph[i].p_memsz;
	unsigned char *img = aligned_alloc(65536, (end + 65535) & ~(size_t)65535);
	memset(img, 0, end);
	for (int i = 0; i < eh->e_phnum; i++)
		if (ph[i].p_type == PT_LOAD)
			memcpy(img + ph[i].p_vaddr, file + ph[i].p_offset, ph[i].p_filesz);
	*len = end;
	*ehp = eh;
	return img;
}

int main(int argc, char **argv)
{
	if (argc != 2) { fprintf(stderr, "usage: %s lib.so\n", argv[0]); return 2; }
	size_t len;
	Elf32_Ehdr *eh;
	unsigned char *img = load(argv[1], &len, &eh);
	Elf32_Phdr *ph = (void *)((char *)eh + eh->e_phoff);
	Elf32_Dyn *dynamic = 0;
	for (int i = 0; i < eh->e_phnum; i++)
		if (ph[i].p_type == PT_DYNAMIC) dynamic = (void *)(img + ph[i].p_vaddr);
	CHECK(dynamic, "no PT_DYNAMIC");
	CHECK(!ARCH_EHDR_REJECT(eh), "ARCH_EHDR_REJECT refuses %s", argv[1]);
	{
		Elf32_Ehdr bad = *eh;
		bad.e_machine = EM_X86_64;
		CHECK(ARCH_EHDR_REJECT(&bad), "e_machine not checked");
		bad = *eh; bad.e_flags = (bad.e_flags & ~0xffu) | 2;
		CHECK(ARCH_EHDR_REJECT(&bad), "ABI version not checked");
		bad = *eh; bad.e_ident[EI_CLASS] = ELFCLASS64;
		CHECK(ARCH_EHDR_REJECT(&bad), "ELF class not checked");
	}

	/* 1. the dynamic section */
	struct dso *p = calloc(1, sizeof *p);
	p->base = img;
	p->name = argv[1];
	p->dynv = (Word *)dynamic;
	decode_dyn(p);
	size_t dyn[DYN_CNT], v;
	decode_dynv(p->dynv, dyn, DYN_CNT);
	size_t ndyn = 0;
	for (Elf32_Dyn *d = dynamic; d->d_tag; d++, ndyn++) {
		CHECK(search_dynv(p->dynv, &v, d->d_tag), "search_dynv misses tag %#x", d->d_tag);
		/* search_dynv returns the first entry with the tag */
		Elf32_Dyn *first = dynamic;
		while (first->d_tag != d->d_tag) first++;
		CHECK(v == first->d_un.d_val, "search_dynv tag %#x: %#zx, want %#x",
			d->d_tag, v, first->d_un.d_val);
		if (d->d_tag > 0 && d->d_tag < DYN_CNT && first == d) {
			CHECK(dyn[d->d_tag] == d->d_un.d_val, "decode_dynv tag %d", d->d_tag);
			CHECK(dyn[0] & 1UL << d->d_tag, "decode_dynv mask tag %d", d->d_tag);
		}
	}
	CHECK(!search_dynv(p->dynv, &v, DT_MIPS_GOTSYM), "found an absent tag");
	CHECK(p->syms == (void *)(img + dyn[DT_SYMTAB]), "DT_SYMTAB");
	CHECK(p->strings == (void *)(img + dyn[DT_STRTAB]), "DT_STRTAB");
	CHECK(p->hashtab && p->ghashtab, "need both .hash and .gnu.hash (--hash-style=both)");
	printf("dynamic section: %zu entries\n", ndyn);

	/* 2. symbol count: .hash nchain, and .gnu.hash alone */
	size_t nsym = p->hashtab[1];
	CHECK(count_syms(p) == nsym, "count_syms from .hash");
	Elf_Symndx *ht = p->hashtab;
	p->hashtab = 0;
	CHECK(count_syms(p) == nsym, "count_syms from .gnu.hash: %zu, want %zu",
		count_syms(p), nsym);
	p->hashtab = ht;
	printf("symbols: %zu, bloom words: %u x %zu bits\n", nsym, p->ghashtab[2],
		8 * sizeof(Word));

	/* 3. lookups through both hash tables */
	uint32_t *gh = p->ghashtab;
	head = p;
	size_t found = 0;
	for (size_t i = 1; i < nsym; i++) {
		Sym *s = p->syms + i;
		const char *name = p->strings + s->st_name;
		if (!s->st_shndx || !*name) continue;
		if (!(1 << (s->st_info & 0xf) & OK_TYPES)) continue;
		if (!(1 << (s->st_info >> 4) & OK_BINDS)) continue;
		if (!s->st_value && (s->st_info & 0xf) != STT_TLS) continue;
		struct symdef g = find_sym(p, name, 1);
		p->ghashtab = 0;
		struct symdef h = find_sym(p, name, 1);
		p->ghashtab = gh;
		CHECK(g.sym == s, "GNU hash lookup of %s", name);
		CHECK(h.sym == s, "SysV hash lookup of %s", name);
		found++;
	}
	static const char *absent[] = { "no_such_symbol", "mallocx", "_dlstart_c", "__dls3x" };
	for (size_t i = 0; i < sizeof absent / sizeof *absent; i++) {
		CHECK(!find_sym(p, absent[i], 1).sym, "GNU hash finds %s", absent[i]);
		p->ghashtab = 0;
		CHECK(!find_sym(p, absent[i], 1).sym, "SysV hash finds %s", absent[i]);
		p->ghashtab = gh;
	}
	printf("lookups: %zu defined symbols found through both hashes\n", found);

	/* 4. relocate the image; check every field independently */
	unsigned char *before = malloc(len);
	memcpy(before, img, len);
	unsigned char *touched = calloc(len, 1);
	static jmp_buf jb;
	rtld_fail = &jb;
	if (setjmp(jb)) { CHECK(0, "do_relocs longjmp"); return 1; }
	do_relocs(p, laddr(p, dyn[DT_JMPREL]), dyn[DT_PLTRELSZ],
		2+(dyn[DT_PLTREL]==DT_RELA));
	do_relocs(p, laddr(p, dyn[DT_RELA]), dyn[DT_RELASZ], 3);
	CHECK(!errors_seen, "loader errors");
	size_t counts[256] = {0};
	struct { size_t at, sz; } tabs[2] = {
		{ dyn[DT_RELA], dyn[DT_RELASZ] }, { dyn[DT_JMPREL], dyn[DT_PLTRELSZ] } };
	CHECK(dyn[DT_RELAENT] == sizeof(Elf32_Rela), "DT_RELAENT %zu", dyn[DT_RELAENT]);
	for (int t = 0; t < 2; t++) {
		Elf32_Rela *r = (void *)(img + tabs[t].at);
		for (size_t k = 0; k < tabs[t].sz / sizeof *r; k++) {
			unsigned type = ELF32_R_TYPE(r[k].r_info);
			unsigned symi = ELF32_R_SYM(r[k].r_info);
			int64_t a = r[k].r_addend;
			uint64_t want, got;
			Elf32_Sym *s = (Elf32_Sym *)p->syms + symi;
			counts[type]++;
			switch (type) {
			case 15: want = (uint64_t)(uintptr_t)img + a; break;
			case 2: case 13: case 14:
				want = (uint64_t)(uintptr_t)img + s->st_value + a; break;
			default:
				CHECK(0, "unexpected type %u in %s", type, argv[1]);
				continue;
			}
			CHECK(r[k].r_offset % 8 == 0, "unaligned field %#x", r[k].r_offset);
			memcpy(&got, img + r[k].r_offset, 8);
			CHECK(got == want, "type %u at %#x: %#llx, want %#llx", type,
				r[k].r_offset, (unsigned long long)got, (unsigned long long)want);
			memset(touched + r[k].r_offset, 1, 8);
		}
	}
	size_t other = 0;
	for (size_t i = 0; i < len; i++)
		if (!touched[i] && img[i] != before[i]) other++;
	CHECK(!other, "%zu bytes changed outside relocation targets", other);
	printf("relocations: RELATIVE %zu, GLOB_DAT %zu, JUMP_SLOT %zu, ABS64 %zu"
		" — all fields as expected\n", counts[15], counts[13], counts[14], counts[2]);

	/* 5. synthetic table: sign-extended addends, TLS, COPY */
	{
		static uint64_t field[8] __attribute__((aligned(8)));
		static unsigned char src[24] = "0123456789abcdefghijklm";
		static Elf32_Sym syms[4];
		static char strings[] = "\0tlsvar\0datum\0";
		static uint32_t sysv[2 + 1 + 4] = { 1, 4, 0 };
		struct dso *q = calloc(1, sizeof *q), *lib = calloc(1, sizeof *lib);
		unsigned char *qbase = (unsigned char *)field - 0x1000;
		/* q: the module being relocated; lib: defines tlsvar and datum */
		q->base = qbase; q->name = "synthetic"; q->syms = (Sym *)syms;
		static uint32_t qhash[2 + 1 + 4] = { 1, 4 };	/* no symbols */
		q->strings = strings; q->hashtab = qhash; q->ghashtab = 0;
		syms[1] = (Elf32_Sym){ .st_name = 1, .st_info = ELF32_ST_INFO(STB_GLOBAL, STT_TLS) };
		/* COPY copies the referencing symbol's st_size */
		syms[2] = (Elf32_Sym){ .st_name = 8, .st_size = 24,
			.st_info = ELF32_ST_INFO(STB_GLOBAL, STT_OBJECT) };
		static Elf32_Sym lsyms[4];
		lsyms[1] = (Elf32_Sym){ .st_name = 1, .st_value = 0x30, .st_size = 8,
			.st_shndx = 1, .st_info = ELF32_ST_INFO(STB_GLOBAL, STT_TLS) };
		lsyms[2] = (Elf32_Sym){ .st_name = 8, .st_value = 0x2000, .st_size = 24,
			.st_shndx = 1, .st_info = ELF32_ST_INFO(STB_GLOBAL, STT_OBJECT) };
		/* .hash: 1 bucket -> 2 -> 1 -> 0 */
		sysv[2] = 2; sysv[3+2] = 1; sysv[3+1] = 0;
		lib->base = src - 0x2000; lib->name = "lib"; lib->syms = (Sym *)lsyms;
		lib->strings = strings; lib->hashtab = sysv;
		lib->tls_id = 2; lib->tls.offset = 0x140;
		static_tls_cnt = 2;
		/* COPY looks up from head->syms_next */
		head = q; q->syms_next = lib;
		Elf32_Rela rela[6] = {
			{ 0x1000, ELF32_R_INFO(0, 15), -8 },	/* RELATIVE, negative */
			{ 0x1008, ELF32_R_INFO(2, 2), -0x10 },	/* ABS64 datum-16 */
			{ 0x1010, ELF32_R_INFO(1, 16), 4 },	/* TPOFF64 tlsvar+4 */
			{ 0x1018, ELF32_R_INFO(1, 17), 0 },	/* DTPMOD64 */
			{ 0x1020, ELF32_R_INFO(1, 18), 8 },	/* DTPOFF64 tlsvar+8 */
			{ 0x1028, ELF32_R_INFO(2, 12), 0 },	/* COPY datum, 24 bytes */
		};
		memset(field, 0xa5, sizeof field);
		do_relocs(q, (Word *)rela, sizeof rela, 3);
		CHECK(!errors_seen, "loader errors (synthetic)");
		CHECK(field[0] == (uint64_t)(uintptr_t)qbase - 8, "RELATIVE -8: %#llx",
			(unsigned long long)field[0]);
		CHECK(field[1] == (uint64_t)(uintptr_t)src - 0x10, "ABS64 -16: %#llx",
			(unsigned long long)field[1]);
		CHECK(field[2] == (uint64_t)(0x30 - 0x140 + 4), "TPOFF64: %#llx",
			(unsigned long long)field[2]);
		CHECK((int64_t)field[2] == -0x10c, "TPOFF64 is not a negative 64-bit value");
		CHECK(field[3] == 2, "DTPMOD64: %#llx", (unsigned long long)field[3]);
		CHECK(field[4] == 0x38, "DTPOFF64: %#llx", (unsigned long long)field[4]);
		CHECK(!memcmp(&field[5], src, 24), "COPY");
		printf("synthetic: negative addends, TPOFF64/DTPMOD64/DTPOFF64, COPY ok\n");
	}

	if (failures) { printf("%d FAILURES\n", failures); return 1; }
	printf("dynlink host test: all passed\n");
	return 0;
}

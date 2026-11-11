/*
 * dt-main: the M5 dynamic-linking test PIE (DYNLINK_PLAN.md).  Linked
 * against libdt1.so (and libc.so), -rdynamic; loads libdt2.so with dlopen.
 *
 *   dt-main core | core-direct | tls | dlopen | misc | preload | ldlp | badelf
 *
 * Every check prints "PASS <name>: ..." or "FAIL <name>: ..."; the last
 * line is "DONE <mode> failures=<n>", and the exit status is 0 only
 * without failures.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <semaphore.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include "dt1.h"

extern char **environ;

/* ---- reporting ---- */

static int failures;

static void check(const char *name, int ok, const char *fmt, ...)
{
	va_list ap;

	printf("%s %s: ", ok ? "PASS" : "FAIL", name);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	if (!ok)
		failures++;
}

#define OPAQUE(p) ({ __typeof__(p) _p = (p); __asm__ volatile("" : "+r"(_p)); _p; })

/* ---- the PIE's own definitions ---- */

/* Interposition: these are also defined by libdt1.so. */
int dt1_ivar = 2;
int dt1_interp(void) { return 'P'; }
int dt1_prot_fn(void) { return 'P'; }
int dt1_weak_fn(void) { return 2; }

/* Called back by libdt2.so (exported: -rdynamic). */
int pie_cb_count;
int pie_exported_data = 4242;
int pie_callback(int x) { pie_cb_count++; return x * 2; }

/* Own TLS: local exec. */
__thread int pie_tls = 77;
__thread char pie_tls_arr[100] __attribute__((aligned(32))) = { 1 };
static __thread int pie_tls_bss;

/* Own .data.rel.ro: RELATIVE relocations in the PIE. */
static int pie_arr[4] = { 10, 20, 30, 40 };
int *const pie_arr_m1 = (int *)((char *)pie_arr - sizeof(int));	/* &pie_arr[-1] */
static const char *const pie_names[] = { "one", "two", "three" };
static int pie_f1(void) { return 11; }
static int pie_f2(void) { return 22; }
static int (*const pie_fns[])(void) = { pie_f1, pie_f2 };

extern const Elf32_Dyn _DYNAMIC[] __attribute__((visibility("hidden")));

static int pie_ctor_ran;
__attribute__((constructor)) static void pie_ctor(void) { pie_ctor_ran++; dt1_logc('P'); }
__attribute__((destructor)) static void pie_dtor(void) { dt1_logc('p'); }
static void pie_atexit(void) { dt1_logc('e'); }

/* ---- dl_iterate_phdr ---- */

struct mod {
	char name[96];
	unsigned long addr;
	const Elf32_Phdr *phdr;
	int phnum;
	int has_tls, has_dyn, has_phdr, has_relro;
	unsigned long tls_memsz, tls_align, dyn_vaddr, phdr_vaddr;
	size_t modid;
	void *tls_data;
	unsigned long long adds, subs;
};
static struct mod mods[16];
static int nmods;
static size_t phdr_info_size;

static int phdr_cb(struct dl_phdr_info *info, size_t size, void *data)
{
	struct mod *m;

	(void)data;
	phdr_info_size = size;
	if (nmods >= 16)
		return 1;
	m = &mods[nmods++];
	memset(m, 0, sizeof(*m));
	snprintf(m->name, sizeof(m->name), "%s", info->dlpi_name ? info->dlpi_name : "(null)");
	m->addr = info->dlpi_addr;
	m->phdr = info->dlpi_phdr;
	m->phnum = info->dlpi_phnum;
	m->modid = info->dlpi_tls_modid;
	m->tls_data = info->dlpi_tls_data;
	m->adds = info->dlpi_adds;
	m->subs = info->dlpi_subs;
	for (int i = 0; i < info->dlpi_phnum; i++) {
		const Elf32_Phdr *ph = &info->dlpi_phdr[i];

		switch (ph->p_type) {
		case PT_TLS:
			m->has_tls = 1;
			m->tls_memsz = ph->p_memsz;
			m->tls_align = ph->p_align;
			break;
		case PT_DYNAMIC:
			m->has_dyn = 1;
			m->dyn_vaddr = ph->p_vaddr;
			break;
		case PT_PHDR:
			m->has_phdr = 1;
			m->phdr_vaddr = ph->p_vaddr;
			break;
		case PT_GNU_RELRO:
			m->has_relro = 1;
			break;
		}
	}
	return 0;
}

static void collect_mods(void)
{
	nmods = 0;
	dl_iterate_phdr(phdr_cb, NULL);
}

static struct mod *find_mod(const char *sub)
{
	for (int i = 0; i < nmods; i++)
		if (strstr(mods[i].name, sub))
			return &mods[i];
	return NULL;
}

static struct mod *libc_mod(void)
{
	struct mod *m = find_mod("libc.so");

	return m ? m : find_mod("ld-musl");
}

static int in_block(const void *p, const void *base, unsigned long len)
{
	return (uintptr_t)p >= (uintptr_t)base && (uintptr_t)p < (uintptr_t)base + len;
}

static int is_elf_at(unsigned long a)
{
	return a && !memcmp((const void *)a, ELFMAG, SELFMAG);
}

static void print_mods(void)
{
	for (int i = 0; i < nmods; i++)
		printf("INFO module %d: %s addr=%#lx phnum=%d tls=%d modid=%zu tls_data=%p dyn=%d relro=%d adds=%llu subs=%llu\n",
		       i, mods[i].name, mods[i].addr, mods[i].phnum, mods[i].has_tls, mods[i].modid,
		       mods[i].tls_data, mods[i].has_dyn, mods[i].has_relro, mods[i].adds, mods[i].subs);
}

/* ---- core: data, functions, relocations, auxv, dl_iterate_phdr, dladdr ---- */

static int test_core(int argc, char **argv, char **envp, int direct)
{
	Dl_info di;
	unsigned long sum;
	int n;

	dt1_report_dtors = 1;
	atexit(pie_atexit);

	/* argv / envp */
	n = 0;
	while (envp[n])
		n++;
	check("argv", argc >= 2 && argv[argc] == NULL && strstr(argv[0], "dt-main") && !strncmp(argv[1], "core", 4),
	      "argc=%d argv[0]=%s argv[1]=%s", argc, argv[0], argc > 1 ? argv[1] : "-");
	check("envp", envp == environ && envp[n] == NULL, "envp=%p environ=%p count=%d first=%s",
	      (void *)envp, (void *)environ, n, n ? envp[0] : "-");

	/* Constructors so far: the library's (by priority), then the PIE's. */
	check("ctor-order", !strcmp(dt1_log, "abcP") && pie_ctor_ran == 1, "log=\"%s\" want \"abcP\"", dt1_log);

	/* GLOB_DAT: one object seen from both modules. */
	check("globdat-read", dt1_counter == 100 && dt1_get_counter() == 100, "dt1_counter=%d", dt1_counter);
	dt1_counter = 123;
	check("globdat-write-pie", dt1_get_counter() == 123, "lib sees %d after PIE wrote 123", dt1_get_counter());
	dt1_set_counter(456);
	check("globdat-write-lib", dt1_counter == 456, "PIE sees %d after lib wrote 456", dt1_counter);
	check("globdat-same-object", &dt1_counter == dt1_counter_addr(), "PIE &=%p lib &=%p",
	      (void *)&dt1_counter, (void *)dt1_counter_addr());

	/* Function pointer table in .data (ABS64 for preemptible, RELATIVE for static). */
	check("fptr-table", dt1_ops[0](3, 4) == 7 && dt1_ops[1](3, 4) == -1 && dt1_ops[2](3, 4) == 12 &&
	      dt1_ops[3](3, 4) == -3 && dt1_call_op(2, 5, 6) == 30,
	      "ops: %d %d %d %d", dt1_ops[0](3, 4), dt1_ops[1](3, 4), dt1_ops[2](3, 4), dt1_ops[3](3, 4));
	check("fptr-equality", OPAQUE(&dt1_ops[0])[0] == dt1_add && OPAQUE(&dt1_ops[0])[1] == dt1_sub,
	      "dt1_ops[0]=%p &dt1_add=%p", (void *)dt1_ops[0], (void *)dt1_add);

	/* .data.rel.ro of the library. */
	check("relro-ptrs", !strcmp(OPAQUE(&dt1_names[0])[2], "mul") && !strcmp(OPAQUE(&dt1_names[0])[0], "add") &&
	      OPAQUE(&dt1_cptr)[0] == &dt1_counter && *dt1_cptr == 456,
	      "names[2]=%s cptr=%p &counter=%p", dt1_names[2], (void *)dt1_cptr, (void *)&dt1_counter);

	/* .data.rel.ro of the PIE. */
	check("pie-relro-ptrs", OPAQUE(&pie_arr_m1)[0][1] == 10 && !strcmp(OPAQUE(&pie_names[0])[2], "three") &&
	      OPAQUE(&pie_fns[0])[0]() == 11 && OPAQUE(&pie_fns[0])[1]() == 22,
	      "pie_arr_m1[1]=%d names[2]=%s", OPAQUE(&pie_arr_m1)[0][1], OPAQUE(&pie_names[0])[2]);

	/* Stack arguments and varargs through the PLT. */
	check("plt-stack-args", dt1_sum6(1, 2, 3, 4, 5, 6) == 91 && dt1_vsum(5, 1L, 2L, 3L, 4L, 5L) == 12345,
	      "sum6=%ld vsum=%ld", dt1_sum6(1, 2, 3, 4, 5, 6), dt1_vsum(5, 1L, 2L, 3L, 4L, 5L));

	/* Protected data: reached through the PIE's GOT, never copied. */
	check("protected-data", dt1_prot_data == 7 && &dt1_prot_data == dt1_prot_data_addr(),
	      "value=%d PIE &=%p lib &=%p", dt1_prot_data, (void *)&dt1_prot_data, (void *)dt1_prot_data_addr());
	dt1_set_prot_data(8);
	check("protected-data-write", dt1_prot_data == 8, "PIE sees %d after lib wrote 8", dt1_prot_data);

	/* Interposition. */
	check("interpose-func", dt1_call_interp() == 'P', "lib's call to dt1_interp reached '%c' (want 'P')",
	      dt1_call_interp());
	check("interpose-data", dt1_get_ivar() == 2 && dt1_ivar_addr() == &dt1_ivar,
	      "lib reads dt1_ivar=%d (want the PIE's 2); lib &=%p PIE &=%p", dt1_get_ivar(),
	      (void *)dt1_ivar_addr(), (void *)&dt1_ivar);
	check("protected-func", dt1_call_prot() == 'L' && dt1_prot_fn() == 'P',
	      "lib's call to protected dt1_prot_fn reached '%c' (want 'L'); PIE's own '%c'", dt1_call_prot(),
	      dt1_prot_fn());
	check("weak-def-interposed", dt1_call_weak() == 2, "lib's call to weak dt1_weak_fn gave %d (want the PIE's 2)",
	      dt1_call_weak());
	check("weak-def", dt1_weak2() == 5, "dt1_weak2()=%d", dt1_weak2());
	check("weak-undef", dt1_weak_undef_is_null(), "weak undefined function and data are NULL in the library");

	/* > 64 KiB .bss and .data across pages. */
	check("big-bss-zero", dt1_big_all_zero(), "dt1_big (200000 bytes) zero at start");
	sum = 0;
	for (size_t i = 0; i < sizeof(dt1_big); i++) {
		dt1_big[i] = (char)(i * 7 + (i >> 16));
		sum = sum * 31 + (unsigned char)dt1_big[i];
	}
	check("big-bss-shared", dt1_big_sum() == sum, "lib checksum %#lx PIE %#lx", dt1_big_sum(), sum);
	check("big-data", dt1_bigdata[0] == 0x1234 && dt1_bigdata[10000] == 0x5678 && dt1_bigdata[19999] == 0x9abc &&
	      dt1_bigdata[1] == 0 && dt1_bigdata[19998] == 0,
	      "[0]=%#x [10000]=%#x [19999]=%#x", dt1_bigdata[0], dt1_bigdata[10000], dt1_bigdata[19999]);

	/* auxv and dl_iterate_phdr. */
	unsigned long at_phdr = getauxval(AT_PHDR), at_base = getauxval(AT_BASE), at_entry = getauxval(AT_ENTRY);
	unsigned long at_phnum = getauxval(AT_PHNUM), at_pagesz = getauxval(AT_PAGESZ);

	printf("INFO auxv: AT_PHDR=%#lx AT_PHNUM=%lu AT_BASE=%#lx AT_ENTRY=%#lx AT_PAGESZ=%#lx\n", at_phdr, at_phnum,
	       at_base, at_entry, at_pagesz);
	collect_mods();
	print_mods();
	check("phdr-info-size", phdr_info_size == sizeof(struct dl_phdr_info) && sizeof(struct dl_phdr_info) == 64 &&
	      offsetof(struct dl_phdr_info, dlpi_phnum) == 24 && offsetof(struct dl_phdr_info, dlpi_adds) == 32 &&
	      offsetof(struct dl_phdr_info, dlpi_tls_data) == 56,
	      "size arg=%zu sizeof=%zu", phdr_info_size, sizeof(struct dl_phdr_info));

	struct mod *pie = nmods ? &mods[0] : NULL, *d1 = find_mod("libdt1.so"), *lc = libc_mod();

	check("iterate-count", nmods == 3 && pie && d1 && lc, "%d modules: want PIE, libdt1.so, libc", nmods);
	if (pie) {
		const Elf32_Ehdr *eh = (const Elf32_Ehdr *)pie->addr;

		check("pie-base", is_elf_at(pie->addr) && (direct || pie->addr == 0x55550000UL),
		      "dlpi_addr=%#lx name=%s (kernel-loaded PIE without ASLR: 0x55550000)", pie->addr, pie->name);
		/* Loader run as a command: musl's getauxval still returns the
		 * kernel's vector, which describes the loader (upstream
		 * behaviour, not rewritten as glibc does): only dlpi_phdr. */
		check("pie-at-phdr", pie->has_phdr && (unsigned long)pie->phdr == pie->addr + pie->phdr_vaddr &&
		      (direct || ((unsigned long)pie->phdr == at_phdr && at_phnum == (unsigned long)pie->phnum)),
		      "AT_PHDR=%#lx dlpi_phdr=%p addr+PT_PHDR=%#lx", at_phdr, (void *)pie->phdr,
		      pie->addr + pie->phdr_vaddr);
		check("pie-at-entry", is_elf_at(pie->addr) && (direct || at_entry == pie->addr + eh->e_entry),
		      "AT_ENTRY=%#lx base+e_entry=%#lx%s", at_entry, pie->addr + eh->e_entry,
		      direct ? " (loader run as a command: auxv describes the loader, not compared)" : "");
		check("pie-dynamic", pie->has_dyn && pie->addr + pie->dyn_vaddr == (unsigned long)_DYNAMIC,
		      "addr+PT_DYNAMIC=%#lx _DYNAMIC=%p", pie->addr + pie->dyn_vaddr, (void *)_DYNAMIC);
		check("pie-tls-phdr", pie->has_tls && pie->modid == 1 && in_block(&pie_tls, pie->tls_data, pie->tls_memsz) &&
		      in_block(pie_tls_arr, pie->tls_data, pie->tls_memsz),
		      "modid=%zu tls_data=%p memsz=%lu &pie_tls=%p", pie->modid, pie->tls_data, pie->tls_memsz,
		      (void *)&pie_tls);

		/* DT_DEBUG -> r_debug (what a debugger reads). */
		struct r_debug *rd = NULL;

		for (const Elf32_Dyn *d = _DYNAMIC; d->d_tag != DT_NULL; d++)
			if (d->d_tag == DT_DEBUG)
				rd = (struct r_debug *)(uintptr_t)d->d_un.d_val;
		check("r-debug", rd && rd->r_version == 1 && rd->r_map && rd->r_map->l_addr == pie->addr &&
		      (direct || rd->r_ldbase == at_base) && rd->r_map->l_next &&
		      rd->r_map->l_next->l_addr == (d1 ? d1->addr : 0),
		      "DT_DEBUG=%p version=%d map=%p l_addr=%#lx ldbase=%#lx", (void *)rd, rd ? rd->r_version : -1,
		      rd ? (void *)rd->r_map : NULL, rd && rd->r_map ? (unsigned long)rd->r_map->l_addr : 0UL,
		      rd ? (unsigned long)rd->r_ldbase : 0UL);
	}
	if (d1) {
		check("dt1-base", is_elf_at(d1->addr) && d1->has_dyn && d1->has_relro &&
		      ((const Elf32_Dyn *)(d1->addr + d1->dyn_vaddr))->d_tag == DT_NEEDED,
		      "dlpi_addr=%#lx PT_DYNAMIC at %#lx", d1->addr, d1->addr + d1->dyn_vaddr);
		check("dt1-tls-phdr", d1->has_tls && d1->modid >= 2 && in_block(&dt1_tls, d1->tls_data, d1->tls_memsz) &&
		      in_block(&dt1_tls_aligned, d1->tls_data, d1->tls_memsz) && d1->tls_align == 64,
		      "modid=%zu tls_data=%p memsz=%lu align=%lu &dt1_tls=%p", d1->modid, d1->tls_data, d1->tls_memsz,
		      d1->tls_align, (void *)&dt1_tls);
		check("dladdr-lib", dladdr((void *)dt1_add, &di) && di.dli_fname && strstr(di.dli_fname, "libdt1.so") &&
		      (unsigned long)di.dli_fbase == d1->addr && di.dli_sname && !strcmp(di.dli_sname, "dt1_add") &&
		      di.dli_saddr == (void *)dt1_add,
		      "fname=%s fbase=%p sname=%s saddr=%p", di.dli_fname, di.dli_fbase, di.dli_sname, di.dli_saddr);
		check("dladdr-lib-data", dladdr((void *)&dt1_bigdata[10000], &di) && di.dli_sname &&
		      !strcmp(di.dli_sname, "dt1_bigdata") && di.dli_saddr == (void *)dt1_bigdata,
		      "sname=%s saddr=%p", di.dli_sname, di.dli_saddr);
	}
	if (lc) {
		check("libc-base", is_elf_at(lc->addr) && lc->has_dyn && (direct ? 1 : lc->addr == at_base),
		      "name=%s dlpi_addr=%#lx AT_BASE=%#lx%s", lc->name, lc->addr, at_base,
		      direct ? " (loader run as a command: AT_BASE not compared)" : "");
		check("dladdr-libc", dladdr((void *)printf, &di) && di.dli_sname && !strcmp(di.dli_sname, "printf") &&
		      (unsigned long)di.dli_fbase == lc->addr,
		      "fname=%s sname=%s fbase=%p", di.dli_fname, di.dli_sname, di.dli_fbase);
	}
	check("dladdr-pie", dladdr((void *)pie_callback, &di) && di.dli_sname && !strcmp(di.dli_sname, "pie_callback") &&
	      pie && (unsigned long)di.dli_fbase == pie->addr,
	      "fname=%s sname=%s fbase=%p", di.dli_fname, di.dli_sname, di.dli_fbase);
	check("at-base", direct ? 1 : (at_base != 0), "AT_BASE=%#lx", at_base);
	check("at-pagesz", at_pagesz == 65536, "AT_PAGESZ=%lu", at_pagesz);
	return 0;
}

/* ---- tls: 4 threads, library GD + PIE IE + PIE LE, errno ---- */

#define NT 4
static pthread_barrier_t bar;
struct tres {
	int ok;
	char msg[160];
	void *a_pie, *a_dt1, *a_err, *a_dt1bss;
};
static struct tres tr[NT + 1];

#define TFAIL(r, ...) do { if ((r)->ok) { (r)->ok = 0; snprintf((r)->msg, sizeof((r)->msg), __VA_ARGS__); } } while (0)

static void tls_body(int i, struct tres *r, int threaded)
{
	r->ok = 1;
	if (pie_tls != 77 || pie_tls_arr[0] != 1 || pie_tls_bss != 0)
		TFAIL(r, "initial PIE TLS %d %d %d", pie_tls, pie_tls_arr[0], pie_tls_bss);
	if ((uintptr_t)pie_tls_arr % 32)
		TFAIL(r, "pie_tls_arr misaligned %p", (void *)pie_tls_arr);
	if (dt1_tls != 5 || dt1_tls_get() != 5)
		TFAIL(r, "initial dt1_tls IE=%d GD=%d", dt1_tls, dt1_tls_get());
	if (&dt1_tls != dt1_tls_addr())
		TFAIL(r, "IE &dt1_tls=%p GD=%p", (void *)&dt1_tls, (void *)dt1_tls_addr());
	if (dt1_tls_aligned != 0x5a5a || (uintptr_t)&dt1_tls_aligned % 64 || &dt1_tls_aligned != dt1_tls_aligned_addr())
		TFAIL(r, "dt1_tls_aligned %#lx at %p (GD %p)", dt1_tls_aligned, (void *)&dt1_tls_aligned,
		      (void *)dt1_tls_aligned_addr());
	if (dt1_tls_bss != 0 || &dt1_tls_bss != dt1_tls_bss_addr())
		TFAIL(r, "dt1_tls_bss %d at %p (GD %p)", dt1_tls_bss, (void *)&dt1_tls_bss, (void *)dt1_tls_bss_addr());
	pie_tls = 1000 + i;
	dt1_tls = 2000 + i;
	dt1_tls_bss = 3000 + i;
	errno = 4000 + i;
	pie_tls_bss = 6000 + i;
	if (threaded)
		pthread_barrier_wait(&bar);
	if (dt1_tls_get() != 2000 + i)
		TFAIL(r, "GD read %d after IE write %d", dt1_tls_get(), 2000 + i);
	dt1_tls_set(5000 + i);
	if (threaded)
		pthread_barrier_wait(&bar);
	if (dt1_tls != 5000 + i)
		TFAIL(r, "IE read %d after GD write %d", dt1_tls, 5000 + i);
	if (pie_tls != 1000 + i || *dt1_tls_bss_addr() != 3000 + i || errno != 4000 + i || pie_tls_bss != 6000 + i)
		TFAIL(r, "clobbered: pie_tls=%d dt1_tls_bss=%d errno=%d pie_tls_bss=%d", pie_tls, *dt1_tls_bss_addr(),
		      errno, pie_tls_bss);
	if (dt1_errno_addr() != &errno)
		TFAIL(r, "lib &errno %p PIE %p", (void *)dt1_errno_addr(), (void *)&errno);
	r->a_pie = &pie_tls;
	r->a_dt1 = &dt1_tls;
	r->a_err = &errno;
	r->a_dt1bss = &dt1_tls_bss;
}

static void *tls_thread(void *arg)
{
	int i = (int)(long)arg;

	tls_body(i, &tr[i], 1);
	return NULL;
}

static int test_tls(void)
{
	pthread_t t[NT];
	int ok = 1;

	pthread_barrier_init(&bar, NULL, NT);
	for (long i = 0; i < NT; i++)
		if (pthread_create(&t[i], NULL, tls_thread, (void *)i))
			check("tls-thread-create", 0, "pthread_create %ld failed", i);
	for (int i = 0; i < NT; i++)
		pthread_join(t[i], NULL);
	tls_body(NT, &tr[NT], 0);	/* the main thread, after the others */
	for (int i = 0; i <= NT; i++) {
		check("tls-thread", tr[i].ok, "%s %d: %s pie_tls@%p dt1_tls@%p errno@%p", i == NT ? "main" : "thread", i,
		      tr[i].ok ? "values ok" : tr[i].msg, tr[i].a_pie, tr[i].a_dt1, tr[i].a_err);
		for (int j = 0; j < i; j++)
			if (tr[i].a_pie == tr[j].a_pie || tr[i].a_dt1 == tr[j].a_dt1 || tr[i].a_err == tr[j].a_err ||
			    tr[i].a_dt1bss == tr[j].a_dt1bss)
				ok = 0;
	}
	check("tls-distinct", ok, "per-thread addresses distinct across %d threads + main", NT);
	/* IE offsets are the same in every thread: the TP offset is per module. */
	ok = 1;
	for (int i = 0; i < NT; i++)
		if ((char *)tr[i].a_dt1 - (char *)tr[i].a_pie != (char *)tr[NT].a_dt1 - (char *)tr[NT].a_pie)
			ok = 0;
	check("tls-static-layout", ok, "dt1_tls - pie_tls constant across threads (static TLS: %td)",
	      (char *)tr[NT].a_dt1 - (char *)tr[NT].a_pie);
	return 0;
}

/* ---- dlopen ---- */

typedef int (*ifn)(void);
static int (*p_dt2_tls_get)(void);
static void (*p_dt2_tls_set)(int);
static int *(*p_dt2_tls_addr)(void);
static int (*p_dt2_tls_arr_ok)(void);
static sem_t go, done;

static void *dl_thread(void *arg)
{
	struct tres *r = arg;
	int i = (int)(r - tr);

	sem_wait(&go);
	r->ok = 1;
	if (p_dt2_tls_get() != 9 || !p_dt2_tls_arr_ok())
		TFAIL(r, "initial dt2_tls=%d arr_ok=%d", p_dt2_tls_get(), p_dt2_tls_arr_ok());
	if (dt1_tls != 5)
		TFAIL(r, "dt1_tls=%d", dt1_tls);
	p_dt2_tls_set(700 + i);
	sem_post(&done);
	sem_wait(&go);
	if (p_dt2_tls_get() != 700 + i || *p_dt2_tls_addr() != 700 + i)
		TFAIL(r, "dt2_tls=%d after writing %d", p_dt2_tls_get(), 700 + i);
	r->a_dt1 = p_dt2_tls_addr();
	sem_post(&done);
	return NULL;
}

static int test_dlopen(void)
{
	void *h, *h2, *hself, *p;
	const char *e;
	pthread_t ta, tb;
	Dl_info di;
	unsigned long long adds0;
	int cb0 = pie_cb_count;

	sem_init(&go, 0, 0);
	sem_init(&done, 0, 0);
	/* Thread A exists before the dlopen. */
	pthread_create(&ta, NULL, dl_thread, &tr[0]);

	collect_mods();
	adds0 = nmods ? mods[0].adds : 0;

	h = dlopen("libnope.so", RTLD_NOW);
	e = dlerror();
	check("dlerror-missing-lib", !h && e && strstr(e, "libnope.so"), "dlopen=%p dlerror=\"%s\"", h, e ? e : "(null)");
	e = dlerror();
	check("dlerror-cleared", !e, "second dlerror=\"%s\"", e ? e : "(null)");

	h = dlopen("libdt2.so", RTLD_NOW | RTLD_LOCAL);
	e = h ? NULL : dlerror();
	check("dlopen", h != NULL, "handle=%p %s", h, e ? e : "");
	if (!h)
		return 0;
	int *ran = dlsym(h, "dt2_ctor_ran"), *ccb = dlsym(h, "dt2_ctor_cb"), *cdt1 = dlsym(h, "dt2_ctor_dt1");

	check("dlopen-ctor", ran && *ran == 1 && ccb && *ccb == 20 && cdt1 && *cdt1 == 42 && pie_cb_count == cb0 + 1,
	      "ran=%d pie_callback(10)=%d dt1_add(20,22)=%d", ran ? *ran : -1, ccb ? *ccb : -1, cdt1 ? *cdt1 : -1);
	int (*dt2_fn)(int) = (int (*)(int))dlsym(h, "dt2_fn");
	ifn rd = (ifn)dlsym(h, "dt2_read_pie_data");

	check("dlsym-call", dt2_fn && dt2_fn(5) == 15 && rd && rd() == 4242, "dt2_fn(5)=%d (want 15) pie data=%d",
	      dt2_fn ? dt2_fn(5) : -1, rd ? rd() : -1);
	p = dlsym(h, "nope_sym");
	e = dlerror();
	check("dlerror-missing-sym", !p && e && strstr(e, "nope_sym"), "dlsym=%p dlerror=\"%s\"", p, e ? e : "(null)");

	check("rtld-default", dlsym(RTLD_DEFAULT, "dt1_add") == (void *)dt1_add &&
	      dlsym(RTLD_DEFAULT, "pie_callback") == (void *)pie_callback,
	      "dt1_add %p/%p pie_callback %p/%p", dlsym(RTLD_DEFAULT, "dt1_add"), (void *)dt1_add,
	      dlsym(RTLD_DEFAULT, "pie_callback"), (void *)pie_callback);
	p = dlsym(RTLD_DEFAULT, "dt2_fn");
	check("rtld-local-hidden", p == NULL, "RTLD_DEFAULT dt2_fn=%p (RTLD_LOCAL: want NULL)", p);
	(void)dlerror();
	h2 = dlopen("/lib/libdt2.so", RTLD_NOW | RTLD_GLOBAL);
	p = dlsym(RTLD_DEFAULT, "dt2_fn");
	check("dlopen-again-global", h2 == h && p == (void *)dt2_fn && *ran == 1,
	      "h2=%p h=%p RTLD_DEFAULT dt2_fn=%p ctor runs=%d", h2, h, p, *ran);
	p = dlsym(RTLD_NEXT, "getpid");
	check("rtld-next-libc", p && p == (void *)getpid && dladdr(p, &di) && di.dli_sname && !strcmp(di.dli_sname, "getpid"),
	      "RTLD_NEXT getpid=%p &getpid=%p in %s", p, (void *)getpid, p && dladdr(p, &di) ? di.dli_fname : "?");
	p = dlsym(RTLD_NEXT, "dt1_add");
	check("rtld-next-lib", p == (void *)dt1_add, "RTLD_NEXT dt1_add=%p &dt1_add=%p", p, (void *)dt1_add);
	hself = dlopen(NULL, RTLD_NOW);
	check("dlopen-null", hself && dlsym(hself, "pie_callback") == (void *)pie_callback &&
	      dlsym(hself, "dt1_add") == (void *)dt1_add,
	      "self=%p pie_callback=%p dt1_add=%p", hself, hself ? dlsym(hself, "pie_callback") : NULL,
	      hself ? dlsym(hself, "dt1_add") : NULL);

	/* TLS of the dlopened library: main, pre-existing thread A, new thread B. */
	p_dt2_tls_get = (int (*)(void))dlsym(h, "dt2_tls_get");
	p_dt2_tls_set = (void (*)(int))dlsym(h, "dt2_tls_set");
	p_dt2_tls_addr = (int *(*)(void))dlsym(h, "dt2_tls_addr");
	p_dt2_tls_arr_ok = (int (*)(void))dlsym(h, "dt2_tls_arr_ok");
	if (!p_dt2_tls_get || !p_dt2_tls_set || !p_dt2_tls_addr || !p_dt2_tls_arr_ok) {
		check("dlsym-tls-fns", 0, "missing");
		return 0;
	}
	check("dlopen-tls-main", p_dt2_tls_get() == 9 && p_dt2_tls_arr_ok() && dlsym(h, "dt2_tls") == p_dt2_tls_addr(),
	      "dt2_tls=%d arr_ok=%d dlsym(dt2_tls)=%p GD &=%p", p_dt2_tls_get(), p_dt2_tls_arr_ok(), dlsym(h, "dt2_tls"),
	      (void *)p_dt2_tls_addr());
	p_dt2_tls_set(600);
	pthread_create(&tb, NULL, dl_thread, &tr[1]);
	sem_post(&go);
	sem_post(&go);
	sem_wait(&done);
	sem_wait(&done);
	sem_post(&go);
	sem_post(&go);
	sem_wait(&done);
	sem_wait(&done);
	pthread_join(ta, NULL);
	pthread_join(tb, NULL);
	check("dlopen-tls-old-thread", tr[0].ok, "thread created before dlopen: %s dt2_tls@%p",
	      tr[0].ok ? "ok" : tr[0].msg, tr[0].a_dt1);
	check("dlopen-tls-new-thread", tr[1].ok, "thread created after dlopen: %s dt2_tls@%p", tr[1].ok ? "ok" : tr[1].msg,
	      tr[1].a_dt1);
	check("dlopen-tls-distinct", p_dt2_tls_get() == 600 && tr[0].a_dt1 != tr[1].a_dt1 &&
	      tr[0].a_dt1 != (void *)p_dt2_tls_addr() && tr[1].a_dt1 != (void *)p_dt2_tls_addr(),
	      "main dt2_tls=%d@%p A@%p B@%p", p_dt2_tls_get(), (void *)p_dt2_tls_addr(), tr[0].a_dt1, tr[1].a_dt1);

	collect_mods();
	print_mods();
	struct mod *d1 = find_mod("libdt1.so"), *d2 = find_mod("libdt2.so");

	check("iterate-dlopen", d2 && d1 && nmods == 4 && d2->has_tls && d2->modid > d1->modid &&
	      in_block(p_dt2_tls_addr(), d2->tls_data, d2->tls_memsz) && mods[0].adds > adds0,
	      "libdt2 modid=%zu tls_data=%p adds %llu->%llu", d2 ? d2->modid : 0, d2 ? d2->tls_data : NULL, adds0,
	      nmods ? mods[0].adds : 0);
	check("dladdr-dlopened", dladdr((void *)dt2_fn, &di) && di.dli_fname && strstr(di.dli_fname, "libdt2.so") &&
	      d2 && (unsigned long)di.dli_fbase == d2->addr,
	      "fname=%s fbase=%p", di.dli_fname, di.dli_fbase);
	struct link_map *lm = NULL;

	check("dlinfo-linkmap", !dlinfo(h, RTLD_DI_LINKMAP, &lm) && lm && d2 && lm->l_addr == d2->addr &&
	      strstr(lm->l_name, "libdt2.so") && (unsigned long)lm->l_ld == d2->addr + d2->dyn_vaddr,
	      "l_addr=%#lx l_name=%s l_ld=%p", lm ? (unsigned long)lm->l_addr : 0UL, lm ? lm->l_name : "-",
	      lm ? (void *)lm->l_ld : NULL);
	check("dlclose", dlclose(h2) == 0 && dlclose(h) == 0, "dlclose returned 0");
	return 0;
}

/* ---- misc: setjmp/longjmp, signals, qsort, errno across modules ---- */

static jmp_buf jb;
static sigjmp_buf sjb;
static volatile int pie_sig_count;
static int cmp_count;

static int cb_jump(int v) { longjmp(jb, v * 10); }
static void pie_sig_handler(int sig) { (void)sig; pie_sig_count++; }
static int cmp_int(const void *a, const void *b)
{
	int x = *(const int *)a, y = *(const int *)b;

	cmp_count++;
	return (x > y) - (x < y);
}

static int child_status(void (*fn)(void))
{
	int st;
	pid_t pid = fork();

	if (pid == 0) {
		fn();
		_exit(0);
	}
	waitpid(pid, &st, 0);
	return st;
}

static void write_lib_relro(void) { *(const char **)OPAQUE((void *)&dt1_names[0]) = "x"; }
static void write_pie_relro(void) { *(const char **)OPAQUE((void *)&pie_names[0]) = "x"; }

static int test_misc(char **envp)
{
	volatile int r;
	struct sigaction sa;
	sigset_t cur;
	int st;

	/* longjmp out of a PIE callback called by the library. */
	r = setjmp(jb);
	if (r == 0) {
		dt1_call_cb(cb_jump, 4);
		check("longjmp-through-lib", 0, "callback returned");
	} else
		check("longjmp-through-lib", r == 50, "setjmp returned %d (want 50)", r);
	/* longjmp called in the library to the PIE's setjmp. */
	r = setjmp(jb);
	if (r == 0) {
		dt1_do_longjmp(&jb, 7);
		check("longjmp-from-lib", 0, "dt1_do_longjmp returned");
	} else
		check("longjmp-from-lib", r == 7, "setjmp returned %d (want 7)", r);
	r = setjmp(jb);
	if (r == 0)
		dt1_do_longjmp(&jb, 0);
	else
		check("longjmp-zero", r == 1, "longjmp(.., 0) gave %d (want 1)", r);

	/* A handler in the library, raised from the PIE. */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = dt1_sig_handler;
	sigaction(SIGUSR1, &sa, NULL);
	raise(SIGUSR1);
	check("signal-lib-handler", dt1_sig_count == 1, "dt1_sig_count=%d", dt1_sig_count);
	/* A handler in the PIE, raised from the library. */
	sa.sa_handler = pie_sig_handler;
	sigaction(SIGUSR2, &sa, NULL);
	r = dt1_raise(SIGUSR2);
	check("signal-pie-handler", pie_sig_count == 1 && r == 0, "pie_sig_count=%d raise=%d", pie_sig_count, r);
	/* siglongjmp out of a library handler to the PIE. */
	sa.sa_handler = dt1_sig_jump;
	sigaction(SIGUSR1, &sa, NULL);
	dt1_jmp_target = &sjb;
	r = sigsetjmp(sjb, 1);
	if (r == 0) {
		raise(SIGUSR1);
		check("siglongjmp-from-lib-handler", 0, "handler returned");
	} else {
		sigprocmask(SIG_SETMASK, NULL, &cur);
		check("siglongjmp-from-lib-handler", r == SIGUSR1 && dt1_sig_count == 2 && !sigismember(&cur, SIGUSR1),
		      "sigsetjmp=%d count=%d SIGUSR1 blocked=%d", r, dt1_sig_count, sigismember(&cur, SIGUSR1));
	}

	/* qsort/bsearch: libc calls back into the PIE. */
	{
		static int a[500];
		unsigned s = 12345;
		int sorted = 1, key, *f;

		for (int i = 0; i < 500; i++) {
			s = s * 1103515245 + 12345;
			a[i] = (int)(s >> 8) - (1 << 22);
		}
		key = a[123];
		qsort(a, 500, sizeof(int), cmp_int);
		for (int i = 1; i < 500; i++)
			if (a[i - 1] > a[i])
				sorted = 0;
		f = bsearch(&key, a, 500, sizeof(int), cmp_int);
		check("qsort-callback", sorted && cmp_count > 500 && f && *f == key, "sorted=%d comparisons=%d bsearch=%p",
		      sorted, cmp_count, (void *)f);
	}

	/* errno: libc's TLS (thread pointer) read from the PIE and the library. */
	errno = 0;
	r = open("/nonexistent/x", O_RDONLY);
	check("errno-libc-pie", r == -1 && errno == ENOENT, "open=%d errno=%d", r, errno);
	r = dt1_errno_after_bad_close();
	check("errno-libc-lib", r == EBADF && errno == EBADF && dt1_errno_addr() == &errno,
	      "lib errno=%d PIE errno=%d lib&=%p PIE&=%p", r, errno, (void *)dt1_errno_addr(), (void *)&errno);
	errno = 0;
	strtol("999999999999999999999999", NULL, 10);
	check("errno-erange", errno == ERANGE, "errno=%d", errno);

	/* libc data through the GOT. */
	FILE **so = dlsym(RTLD_DEFAULT, "stdout");
	char ***en = dlsym(RTLD_DEFAULT, "environ");

	check("libc-data", so && *so == stdout && en && *en == environ && environ == envp,
	      "stdout %p/%p environ %p/%p/%p", so ? (void *)*so : NULL, (void *)stdout, en ? (void *)*en : NULL,
	      (void *)environ, (void *)envp);

	/* RELRO is read-only after loading: writes fault. */
	st = child_status(write_lib_relro);
	check("relro-lib-readonly", WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV, "child status %#x (want SIGSEGV)", st);
	st = child_status(write_pie_relro);
	check("relro-pie-readonly", WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV, "child status %#x (want SIGSEGV)", st);

	/* A dynamic PIE forks and the child keeps working (COW of GOT/data). */
	{
		pid_t pid = fork();

		if (pid == 0)
			_exit(dt1_add(dt1_counter, 1) == 101 && dt1_tls_get() == 5 ? 0 : 1);
		waitpid(pid, &st, 0);
		check("fork-child", WIFEXITED(st) && WEXITSTATUS(st) == 0, "status %#x", st);
	}
	return 0;
}

/* ---- re-exec helpers ---- */

static int run_env(const char *path, char *const argv[], char *const env[], int *status)
{
	pid_t pid = fork();

	if (pid < 0)
		return -1;
	if (pid == 0) {
		execve(path, argv, env);
		printf("execve %s: %s\n", path, strerror(errno));
		_exit(126);
	}
	waitpid(pid, status, 0);
	return 0;
}

static char **env_plus(char **envp, const char *a, const char *b)
{
	int n = 0;
	char **e;

	while (envp[n])
		n++;
	e = calloc(n + 3, sizeof(char *));
	memcpy(e, envp, n * sizeof(char *));
	if (a)
		e[n++] = (char *)a;
	if (b)
		e[n++] = (char *)b;
	e[n] = NULL;
	return e;
}

static int test_preload(char **argv, char **envp)
{
	int st;
	char *av[] = { argv[0], "preload-child", NULL };

	fflush(stdout);
	run_env(argv[0], av, env_plus(envp, "LD_PRELOAD=/lib/libpre.so", "DT_MARK=xyz"), &st);
	check("preload-exec", WIFEXITED(st) && WEXITSTATUS(st) == 0, "child status %#x", st);
	return 0;
}

static int test_preload_child(void)
{
	int *cnt = dlsym(RTLD_DEFAULT, "pre_getpid_count");
	int c0, p1;
	Dl_info di;

	check("preload-loaded", cnt != NULL, "pre_getpid_count=%p", (void *)cnt);
	if (!cnt)
		return 0;
	c0 = *cnt;
	p1 = getpid();
	getpid();
	getpid();
	check("preload-interposes", *cnt - c0 == 3 && p1 == syscall(SYS_getpid), "3 calls counted %d, pid %d/%ld",
	      *cnt - c0, p1, syscall(SYS_getpid));
	check("preload-rtld-next", dladdr(*(void **)dlsym(RTLD_DEFAULT, "pre_real_getpid"), &di) && di.dli_fname &&
	      !strstr(di.dli_fname, "libpre"),
	      "real getpid in %s", di.dli_fname ? di.dli_fname : "?");
	check("preload-env", getenv("DT_MARK") && !strcmp(getenv("DT_MARK"), "xyz"), "DT_MARK=%s",
	      getenv("DT_MARK") ? getenv("DT_MARK") : "(null)");
	collect_mods();
	print_mods();
	check("preload-order", nmods == 4 && strstr(mods[1].name, "libpre.so") && strstr(mods[2].name, "libdt1.so"),
	      "%d modules, [1]=%s [2]=%s", nmods, nmods > 1 ? mods[1].name : "-", nmods > 2 ? mods[2].name : "-");
	return 0;
}

static int test_ldlp(char **envp)
{
	int st;
	char *av[] = { "/bin/dt-ldlp", NULL, NULL };
	char *avr[] = { "/opt/bin/dt-rpath", "rpath-child", NULL };

	fflush(stdout);
	av[1] = "ldlp-no-path";
	run_env(av[0], av, envp, &st);
	check("ldlp-missing", WIFEXITED(st) && WEXITSTATUS(st) == 127,
	      "without LD_LIBRARY_PATH: status %#x (want exit 127, loader error above)", st);
	av[1] = "ldlp-child";
	run_env(av[0], av, env_plus(envp, "LD_LIBRARY_PATH=/opt/dtlib", NULL), &st);
	check("ldlp", WIFEXITED(st) && WEXITSTATUS(st) == 0, "with LD_LIBRARY_PATH=/opt/dtlib: status %#x", st);
	run_env(avr[0], avr, envp, &st);
	check("runpath-origin", WIFEXITED(st) && WEXITSTATUS(st) == 0, "DT_RUNPATH $ORIGIN/../dtlib: status %#x", st);
	av[1] = "ldlp-bad";
	run_env(av[0], av, env_plus(envp, "LD_LIBRARY_PATH=/opt/badlib", NULL), &st);
	check("needed-bad-machine", WIFEXITED(st) && WEXITSTATUS(st) == 127,
	      "DT_NEEDED with wrong e_machine: status %#x (want exit 127, no signal)", st);
	return 0;
}

static int test_badelf(void)
{
	static const struct {
		const char *path, *what;
		int want_ok;
	} t[] = {
		{ "/lib/libdt3-copy.so", "control (unpatched copy)", 1 },
		{ "/lib/libbad-mach.so", "e_machine 0xf3", 0 },
		{ "/lib/libbad-abi.so", "e_flags ABI version 2", 0 },
		{ "/lib/libbad-class.so", "ELFCLASS64", 0 },
		{ "/lib/libbad-trunc.so", "truncated to 200 bytes", 0 },
	};
	char name[64];

	for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		int st;
		pid_t pid;

		fflush(stdout);
		pid = fork();
		if (pid == 0) {
			void *h = dlopen(t[i].path, RTLD_NOW);
			const char *e = h ? NULL : dlerror();

			printf("INFO badelf %s: dlopen=%p dlerror=\"%s\"\n", t[i].path, h, e ? e : "(null)");
			fflush(stdout);
			_exit(h ? 0 : (e ? 1 : 2));
		}
		waitpid(pid, &st, 0);
		snprintf(name, sizeof(name), "badelf-%s", strrchr(t[i].path, '/') + 1);
		check(name, WIFEXITED(st) && WEXITSTATUS(st) == (t[i].want_ok ? 0 : 1), "%s: child status %#x (want %s)",
		      t[i].what, st, t[i].want_ok ? "loaded" : "refused with a dlerror message");
	}
	return 0;
}

int main(int argc, char **argv, char **envp)
{
	const char *mode = argc > 1 ? argv[1] : "core";

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (!strcmp(mode, "core"))
		test_core(argc, argv, envp, 0);
	else if (!strcmp(mode, "core-direct"))
		test_core(argc, argv, envp, 1);
	else if (!strcmp(mode, "tls"))
		test_tls();
	else if (!strcmp(mode, "dlopen"))
		test_dlopen();
	else if (!strcmp(mode, "misc"))
		test_misc(envp);
	else if (!strcmp(mode, "preload"))
		test_preload(argv, envp);
	else if (!strcmp(mode, "preload-child"))
		test_preload_child();
	else if (!strcmp(mode, "ldlp"))
		test_ldlp(envp);
	else if (!strcmp(mode, "badelf"))
		test_badelf();
	else {
		printf("unknown mode %s\n", mode);
		return 2;
	}
	printf("DONE %s failures=%d\n", mode, failures);
	return failures ? 1 : 0;
}

/*
 * libdt1.so: the shared library the M5 test PIE (dtmain.c) is linked
 * against.  Built with -fPIC -shared.
 */
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "dt1.h"

int dt1_counter = 100;
int dt1_ivar = 1;			/* the PIE's dt1_ivar (2) wins */
__attribute__((visibility("protected"))) int dt1_prot_data = 7;

static int dt1_mul(int a, int b) { return a * b; }
static int dt1_neg(int a, int b) { (void)b; return -a; }

int dt1_add(int a, int b) { return a + b; }
int dt1_sub(int a, int b) { return a - b; }

/* .data: dt1_add/dt1_sub preemptible (ABS64), the static ones RELATIVE. */
dt1_op dt1_ops[4] = { dt1_add, dt1_sub, dt1_mul, dt1_neg };

/* .data.rel.ro: string literals (RELATIVE), a pointer to preemptible data
 * (ABS64). */
const char *const dt1_names[4] = { "add", "sub", "mul", "neg" };
int *const dt1_cptr = &dt1_counter;

char dt1_big[200000];
int dt1_bigdata[20000] = { [0] = 0x1234, [10000] = 0x5678, [19999] = 0x9abc };

char dt1_log[64];
int dt1_report_dtors;
volatile int dt1_sig_count;
sigjmp_buf *dt1_jmp_target;

__thread int dt1_tls = 5;
__thread long dt1_tls_aligned __attribute__((aligned(64))) = 0x5a5a;
__thread int dt1_tls_bss;

void dt1_logc(char c)
{
	size_t n = strlen(dt1_log);

	if (n + 1 < sizeof(dt1_log))
		dt1_log[n] = c;
}

/* Constructor order: priority 101, then 200, then the default. */
__attribute__((constructor(101))) static void ctor_a(void) { dt1_logc('a'); }
__attribute__((constructor(200))) static void ctor_b(void) { dt1_logc('b'); }
__attribute__((constructor)) static void ctor_c(void) { dt1_logc('c'); }

/* Destructors run in the opposite order: x (default), y (200), z (101). */
__attribute__((destructor)) static void dtor_x(void) { dt1_logc('x'); }
__attribute__((destructor(200))) static void dtor_y(void) { dt1_logc('y'); }
__attribute__((destructor(101))) static void dtor_z(void)
{
	/* main: "abcP"; exit: atexit 'e', the PIE's destructor 'p', ours. */
	static const char want[] = "abcPepxyz";

	dt1_logc('z');
	if (dt1_report_dtors)
		printf("%s ctor-dtor-order: log=\"%s\" want \"%s\"\n",
		       strcmp(dt1_log, want) ? "FAIL" : "PASS", dt1_log, want);
}

int dt1_get_counter(void) { return dt1_counter; }
int *dt1_counter_addr(void) { return &dt1_counter; }
void dt1_set_counter(int v) { dt1_counter = v; }
int dt1_get_ivar(void) { return dt1_ivar; }
int *dt1_ivar_addr(void) { return &dt1_ivar; }
int dt1_get_prot_data(void) { return dt1_prot_data; }
void dt1_set_prot_data(int v) { dt1_prot_data = v; }
int *dt1_prot_data_addr(void) { return &dt1_prot_data; }

/* Preemptible: the call goes through the PLT and reaches the PIE's. */
__attribute__((noinline)) int dt1_interp(void) { return 'L'; }
int dt1_call_interp(void) { return dt1_interp(); }

/* Protected: bound inside the library whatever the PIE defines. */
__attribute__((visibility("protected"), noinline)) int dt1_prot_fn(void) { return 'L'; }
int dt1_call_prot(void) { return dt1_prot_fn(); }

/* Weak definitions: a strong one in the PIE still wins (ELF dynamic
 * lookup does not look at binding); dt1_weak2 has no other definition. */
__attribute__((weak, noinline)) int dt1_weak_fn(void) { return 1; }
int dt1_call_weak(void) { return dt1_weak_fn(); }
__attribute__((weak, noinline)) int dt1_weak2(void) { return 5; }

extern int dt1_nowhere(void) __attribute__((weak));
extern int dt1_nowhere_data __attribute__((weak));
int dt1_weak_undef_is_null(void)
{
	return dt1_nowhere == 0 && &dt1_nowhere_data == 0;
}

unsigned long dt1_big_sum(void)
{
	unsigned long s = 0;

	for (size_t i = 0; i < sizeof(dt1_big); i++)
		s = s * 31 + (unsigned char)dt1_big[i];
	return s;
}

int dt1_big_all_zero(void)
{
	for (size_t i = 0; i < sizeof(dt1_big); i++)
		if (dt1_big[i])
			return 0;
	return 1;
}

int dt1_call_op(int i, int a, int b) { return dt1_ops[i](a, b); }

int dt1_tls_get(void) { return dt1_tls; }
void dt1_tls_set(int v) { dt1_tls = v; }
int *dt1_tls_addr(void) { return &dt1_tls; }
long *dt1_tls_aligned_addr(void) { return &dt1_tls_aligned; }
int *dt1_tls_bss_addr(void) { return &dt1_tls_bss; }

int dt1_call_cb(int (*cb)(int), int v)
{
	volatile int r = cb(v + 1);	/* may longjmp out of here */

	return r + 1;
}

void dt1_do_longjmp(jmp_buf *b, int v) { longjmp(*b, v); }

void dt1_sig_handler(int sig) { (void)sig; dt1_sig_count++; }

void dt1_sig_jump(int sig)
{
	dt1_sig_count++;
	siglongjmp(*dt1_jmp_target, sig);
}

int dt1_raise(int sig) { return raise(sig); }

int dt1_errno_after_bad_close(void)
{
	errno = 0;
	close(-1);
	return errno;
}

int *dt1_errno_addr(void) { return &errno; }

/* Stack arguments and varargs through the PLT. */
long dt1_sum6(long a, long b, long c, long d, long e, long f)
{
	return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f;
}

long dt1_vsum(int n, ...)
{
	va_list ap;
	long s = 0;

	va_start(ap, n);
	for (int i = 0; i < n; i++)
		s = s * 10 + va_arg(ap, long);
	va_end(ap);
	return s;
}

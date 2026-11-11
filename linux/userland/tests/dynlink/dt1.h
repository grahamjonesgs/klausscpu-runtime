/* libdt1.so's interface (DYNLINK_PLAN M5 tests). */
#ifndef DT1_H
#define DT1_H

#include <setjmp.h>

typedef int (*dt1_op)(int, int);

/* Data the PIE reaches through its GOT (GLOB_DAT). */
extern int dt1_counter;
extern int dt1_ivar;			/* also defined by the PIE: interposed */
extern int dt1_prot_data;		/* protected in the library */
extern dt1_op dt1_ops[4];		/* .data: ABS64 + RELATIVE */
extern const char *const dt1_names[4];	/* .data.rel.ro: RELATIVE */
extern int *const dt1_cptr;		/* .data.rel.ro: ABS64 (preemptible) */
extern char dt1_big[200000];		/* .bss across pages */
extern int dt1_bigdata[20000];		/* .data across pages */
extern char dt1_log[64];		/* constructor / destructor order */
extern int dt1_report_dtors;
extern volatile int dt1_sig_count;
extern sigjmp_buf *dt1_jmp_target;

/* TLS: general dynamic in the library, initial exec in the PIE. */
extern __thread int dt1_tls;
extern __thread long dt1_tls_aligned;	/* aligned(64) */
extern __thread int dt1_tls_bss;

int dt1_add(int, int);
int dt1_sub(int, int);
int dt1_get_counter(void);
int *dt1_counter_addr(void);
void dt1_set_counter(int);
int dt1_get_ivar(void);
int *dt1_ivar_addr(void);
int dt1_get_prot_data(void);
void dt1_set_prot_data(int);
int *dt1_prot_data_addr(void);
int dt1_interp(void);			/* the PIE defines one too */
int dt1_call_interp(void);
int dt1_prot_fn(void);			/* protected; the PIE defines one too */
int dt1_call_prot(void);
int dt1_weak_fn(void);			/* weak in the library; the PIE's is strong */
int dt1_call_weak(void);
int dt1_weak2(void);			/* weak in the library only */
int dt1_weak_undef_is_null(void);
unsigned long dt1_big_sum(void);
int dt1_big_all_zero(void);
int dt1_call_op(int, int, int);
void dt1_logc(char);

int dt1_tls_get(void);
void dt1_tls_set(int);
int *dt1_tls_addr(void);
long *dt1_tls_aligned_addr(void);
int *dt1_tls_bss_addr(void);

int dt1_call_cb(int (*)(int), int);
void dt1_do_longjmp(jmp_buf *, int);
void dt1_sig_handler(int);
void dt1_sig_jump(int);
int dt1_raise(int);
int dt1_errno_after_bad_close(void);
int *dt1_errno_addr(void);
long dt1_sum6(long, long, long, long, long, long);
long dt1_vsum(int, ...);

#endif

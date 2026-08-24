/* Host-side stub of zephyr/kernel.h — just enough to compile vnc_server.c
 * for the hextile/zero-copy harness. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#define ARG_UNUSED(x) (void)(x)

struct k_thread { int dummy; };
typedef struct { int d; } k_timeout_t;
#define K_NO_WAIT  ((k_timeout_t){0})
#define K_MSEC(x)  ((k_timeout_t){0})
#define K_TICKS(x) ((k_timeout_t){0})
#define K_FOREVER  ((k_timeout_t){0})
#define K_THREAD_STACK_DEFINE(name, size) char name[size]
#define K_THREAD_STACK_SIZEOF(name) sizeof(name)
typedef void (*k_thread_entry_t)(void *, void *, void *);

static inline void *k_thread_create(struct k_thread *t, void *stack, size_t sz,
				    k_thread_entry_t e, void *a, void *b,
				    void *c, int prio, uint32_t opts,
				    k_timeout_t delay)
{
	(void)t; (void)stack; (void)sz; (void)e; (void)a; (void)b; (void)c;
	(void)prio; (void)opts; (void)delay;
	return NULL;
}
static inline int k_thread_name_set(void *t, const char *n)
{
	(void)t; (void)n;
	return 0;
}
static inline void k_sleep(k_timeout_t t) { (void)t; }

int64_t k_uptime_get(void);           /* harness */
int printk(const char *fmt, ...);     /* harness */

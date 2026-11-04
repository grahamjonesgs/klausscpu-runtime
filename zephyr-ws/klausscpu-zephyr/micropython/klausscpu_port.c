/* klausscpu_port.c — small things MicroPython's Zephyr port links against
 * that the KlaussCPU Zephyr port doesn't otherwise provide. */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>

/* machine.reset() / sys_reboot().  The CPU has no software reset (a program
 * is restarted by reloading it over the UART or netboot), so stop here. */
void sys_arch_reboot(int type)
{
	ARG_UNUSED(type);
	printk("\nreboot: KlaussCPU has no software reset; halted. Reload the program.\n");
	(void)irq_lock();
	for (;;) {
		k_cpu_idle();
	}
}

/* libm_dbl has no fabs (other ports get it from the toolchain's libm). */
double fabs(double x)
{
	return __builtin_fabs(x);
}

/* Nor log2 (math.log2).  Exact for powers of two, which is what callers
 * check most (log2(1024) == 10); log(x)/ln 2 otherwise, within an ulp or
 * two.  Zero, negatives, inf and nan go through log(). */
double log(double x);
double frexp(double x, int *e);

double log2(double x)
{
	if (x > 0.0 && x <= __DBL_MAX__) {
		int e;
		double m = frexp(x, &e);	/* x = m * 2^e, m in [0.5, 1) */

		if (m == 0.5) {
			return (double)(e - 1);
		}
	}
	return log(x) / 0.693147180559945309417;
}

#if defined(CONFIG_DNS_RESOLVER) && !defined(CONFIG_NET_IPV6)
#include <zephyr/net/dns_resolve.h>

/* Wraps the resolver (-Wl,--wrap=dns_resolve_name, micropython/CMakeLists.txt)
 * for MicroPython's socket.getaddrinfo() (ports/zephyr/modsocket.c), whose
 * callback is dns_resolve_cb.  Other resolver users are passed straight
 * through.
 *
 * - AAAA without IPv6: getaddrinfo(host, port) queries A, then AAAA, and
 *   raises the AAAA query's error (EPFNOSUPPORT) even when A returned
 *   addresses.  Answer AAAA with "no such name" instead: the A results are
 *   returned, and a name with no A record still raises.
 * - A: dns_resolve_cb builds Python objects on the GC heap, and would run on
 *   the resolver's thread, where a collection would scan the wrong stack and
 *   a MemoryError would longjmp into another thread.  Collect the answers
 *   here and replay them on the calling (Python) thread. */
void dns_resolve_cb(enum dns_resolve_status status, struct dns_addrinfo *info,
		    void *user_data) __attribute__((weak));

int __real_dns_resolve_name(struct dns_resolve_context *ctx, const char *query,
			    enum dns_query_type type, uint16_t *dns_id,
			    dns_resolve_cb_t cb, void *user_data, int32_t timeout);

#define MP_DNS_MAX 4

struct mp_dns_sync {
	struct k_sem done;
	enum dns_resolve_status status;
	int n;
	struct dns_addrinfo ai[MP_DNS_MAX];
};

static void mp_dns_collect(enum dns_resolve_status status, struct dns_addrinfo *info,
			   void *user_data)
{
	struct mp_dns_sync *d = user_data;

	if (info == NULL) {
		d->status = status;
		k_sem_give(&d->done);
		return;
	}
	if (d->n < MP_DNS_MAX) {
		d->ai[d->n++] = *info;
	}
}

int __wrap_dns_resolve_name(struct dns_resolve_context *ctx, const char *query,
			    enum dns_query_type type, uint16_t *dns_id,
			    dns_resolve_cb_t cb, void *user_data, int32_t timeout)
{
	if (cb == NULL || cb != dns_resolve_cb) {
		return __real_dns_resolve_name(ctx, query, type, dns_id, cb,
					       user_data, timeout);
	}
	if (type == DNS_QUERY_TYPE_AAAA) {
		cb(DNS_EAI_NONAME, NULL, user_data);
		return 0;
	}

	struct mp_dns_sync d = { .n = 0 };

	k_sem_init(&d.done, 0, 1);
	int ret = __real_dns_resolve_name(ctx, query, type, dns_id, mp_dns_collect,
					  &d, timeout);

	if (ret < 0) {
		return ret;
	}
	/* The resolver always ends a query with a final callback (results
	 * done, an error, or DNS_EAI_CANCELED at the timeout). */
	(void)k_sem_take(&d.done, K_FOREVER);
	for (int i = 0; i < d.n; i++) {
		cb(DNS_EAI_INPROGRESS, &d.ai[i], user_data);
	}
	cb(d.status, NULL, user_data);
	return 0;
}
#endif

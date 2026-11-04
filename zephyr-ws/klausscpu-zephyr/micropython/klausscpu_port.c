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

#if defined(CONFIG_DNS_RESOLVER) && !defined(CONFIG_NET_IPV6)
#include <zephyr/net/dns_resolve.h>

/* socket.getaddrinfo(host, port) with no family queries A, then AAAA, and
 * raises the AAAA query's error (EPFNOSUPPORT without IPv6) even when the A
 * query returned addresses.  Without IPv6, answer AAAA here with "no such
 * name": the A results are returned, and a name with no A record still
 * raises.  (-Wl,--wrap=dns_resolve_name, micropython/CMakeLists.txt.) */
int __real_dns_resolve_name(struct dns_resolve_context *ctx, const char *query,
			    enum dns_query_type type, uint16_t *dns_id,
			    dns_resolve_cb_t cb, void *user_data, int32_t timeout);

int __wrap_dns_resolve_name(struct dns_resolve_context *ctx, const char *query,
			    enum dns_query_type type, uint16_t *dns_id,
			    dns_resolve_cb_t cb, void *user_data, int32_t timeout)
{
	if (type == DNS_QUERY_TYPE_AAAA) {
		cb(DNS_EAI_NONAME, NULL, user_data);
		return 0;
	}
	return __real_dns_resolve_name(ctx, query, type, dns_id, cb, user_data, timeout);
}
#endif

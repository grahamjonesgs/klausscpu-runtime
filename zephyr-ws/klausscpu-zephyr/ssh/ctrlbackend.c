/*
 * ctrlbackend.c — example second loadable HTTP backend, claiming "/ctrl/".
 *
 * Exists to demonstrate the prefix-keyed dispatch (webapi.c): this extension
 * coexists with apibackend.llext (which owns "/api/") — load both and each
 * serves its own namespace concurrently on :80 and :443.  Built as ctrl.llext
 * (add_llext_target in apps/ssh_shell/CMakeLists.txt); load with
 * `svc load /SD:/ctrl.llext`, unload with `svc stop ctrl`.
 *
 *   GET /ctrl/ping          -> {"pong":true,"uptime_ms":N}
 *   GET /ctrl/echo?msg=...   -> {"echo":"..."}   (value reflected verbatim)
 *
 * Same kernel exports as apibackend: httpd_send + webapi_register/unregister,
 * snprintk + str* (ssh/llext_exports.c), k_uptime_get (Zephyr builtin).
 */

#include <zephyr/kernel.h>
#include <string.h>

#include "httpd.h"
#include "webapi.h"

/* Copy the value of `key` from a "k=v&k2=v2" query string into out[outsz]
 * (truncated to fit, NUL-terminated; empty if absent). */
static void query_str(const char *query, const char *key, char *out, size_t outsz)
{
	size_t klen = strlen(key);

	out[0] = '\0';
	for (const char *p = query; p != NULL && *p != '\0';) {
		if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
			const char *v = p + klen + 1;
			size_t i = 0;

			while (v[i] != '\0' && v[i] != '&' && i < outsz - 1) {
				out[i] = v[i];
				i++;
			}
			out[i] = '\0';
			return;
		}
		const char *amp = strchr(p, '&');

		if (amp == NULL) {
			break;
		}
		p = amp + 1;
	}
}

static bool ctrl_handle(struct httpd_conn *c, const char *method,
			const char *url, const char *query)
{
	ARG_UNUSED(method);

	if (strcmp(url, "/ctrl/ping") == 0) {
		(void)snprintk(c->xfer, c->xfer_sz,
			       "{\"pong\":true,\"uptime_ms\":%llu}",
			       (unsigned long long)k_uptime_get());
		httpd_send(c, "200 OK", "application/json", c->xfer);
		return true;
	}

	if (strcmp(url, "/ctrl/echo") == 0) {
		char msg[64];

		/* Demo only: reflected verbatim, not JSON-escaped. */
		query_str(query, "msg", msg, sizeof(msg));
		(void)snprintk(c->xfer, c->xfer_sz, "{\"echo\":\"%s\"}", msg);
		httpd_send(c, "200 OK", "application/json", c->xfer);
		return true;
	}

	httpd_send(c, "404 Not Found", "application/json",
		   "{\"error\":\"unknown ctrl route\"}");
	return true;
}

/* ── service entry points (resolved by the llext loader) ─────────────────── */

int svc_start(void)
{
	return webapi_register("/ctrl/", ctrl_handle) == 0 ? 0 : -1;
}

int svc_stop(void)
{
	webapi_unregister("/ctrl/");
	return 0;
}

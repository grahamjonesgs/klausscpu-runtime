/*
 * webapi.c — dispatcher for the HTTP control/telemetry API (/api/*).
 *
 * The actual route logic (LEDs, 7-seg, status/perf JSON) is NOT here — it lives
 * in a loadable service extension (apibackend.llext) that registers a handler
 * via webapi_register() at `svc load` and removes it at `svc stop`.  This file
 * is the kernel-resident glue that httpd_serve() (shared by the :80 plain and
 * :443 TLS servers) calls: it forwards "/api/..." requests to the registered
 * handler, or answers 503 when no backend is loaded.
 *
 * Concurrency: the :80 and :443 server threads may both be inside the handler
 * at once while a `svc stop` runs on a shell thread.  A refcount (api_inflight)
 * taken around each handler call — plus webapi_unregister() draining it to zero
 * before returning — guarantees the handler code is idle before the extension
 * that owns it is unloaded.  g_handler is swapped only under api_lock, and the
 * inflight bump happens under that same lock, so register/unregister can't race
 * a dispatch.
 */

#include <zephyr/kernel.h>
#include <string.h>
#ifdef CONFIG_LLEXT
#include <zephyr/llext/symbol.h>
#endif

#include "httpd.h"
#include "webapi.h"

static webapi_handler_fn g_handler;
static K_MUTEX_DEFINE(api_lock);
static atomic_t api_inflight;

void webapi_register(webapi_handler_fn fn)
{
	k_mutex_lock(&api_lock, K_FOREVER);
	g_handler = fn;
	k_mutex_unlock(&api_lock);
}

void webapi_unregister(void)
{
	k_mutex_lock(&api_lock, K_FOREVER);
	g_handler = NULL;
	k_mutex_unlock(&api_lock);

	/* Wait for any in-flight handler call (on a server thread) to return
	 * before the caller unloads the extension that owns the handler. */
	while (atomic_get(&api_inflight) != 0) {
		k_sleep(K_MSEC(5));
	}
}

#ifdef CONFIG_LLEXT
EXPORT_SYMBOL(webapi_register);
EXPORT_SYMBOL(webapi_unregister);
#endif

bool webapi_handle(struct httpd_conn *c, const char *method, const char *url,
		   const char *query)
{
	if (strncmp(url, "/api/", 5) != 0) {
		return false;
	}

	/* Snapshot + pin the handler under the lock so a concurrent unregister
	 * can't NULL it (and free its code) between the check and the call. */
	k_mutex_lock(&api_lock, K_FOREVER);
	webapi_handler_fn h = g_handler;

	if (h != NULL) {
		atomic_inc(&api_inflight);
	}
	k_mutex_unlock(&api_lock);

	if (h == NULL) {
		httpd_send(c, "503 Service Unavailable", "application/json",
			   "{\"error\":\"no api backend loaded\"}");
		return true;
	}

	bool ret = h(c, method, url, query);

	atomic_dec(&api_inflight);
	return ret;
}

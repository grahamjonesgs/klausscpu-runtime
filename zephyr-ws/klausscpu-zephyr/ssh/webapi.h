/*
 * webapi.h — pluggable HTTP backend dispatch by URL prefix.
 *
 * webapi.c (kernel-resident) is a *dispatcher*: loadable service extensions
 * register a handler for a URL prefix (e.g. "/api/") via webapi_register(); the
 * dispatcher routes matching requests to the registered handler.  Several
 * backends can be loaded at once (one per prefix), each load/unload-able at
 * runtime without rebuilding the kernel.  A request that matches no registered
 * prefix falls through to normal file serving (404 if there's no such file).
 *
 * Reference backend: apibackend.llext serves "/api/" —
 *   GET /api/status     -> { uptime_ms, switches, leds, seg, perf{...} }
 *   GET /api/leds?v=<n>  -> set LEDs (decimal or 0x hex), returns { leds }
 *   GET /api/seg?v=<n>   -> set 7-seg, returns { seg }
 */
#ifndef KLAUSSCPU_WEBAPI_H_
#define KLAUSSCPU_WEBAPI_H_

#include <stdbool.h>

struct httpd_conn;

/* Backend handler: invoked for a request whose URL matched the handler's
 * registered prefix (longest match wins).  Build/send the response and return
 * true (handled).  Receives the connection + parsed request fields; the handler
 * sees the full url and sub-routes within its namespace. */
typedef bool (*webapi_handler_fn)(struct httpd_conn *c, const char *method,
				  const char *url, const char *query);

/* Dispatch a request to the backend whose registered prefix matches `url`
 * (longest match).  Called by httpd_serve() before file serving: returns true
 * if a backend handled it, false to fall through to the file server. */
bool webapi_handle(struct httpd_conn *c, const char *method, const char *url,
		   const char *query);

/* Register/unregister a backend handler for `prefix` (e.g. "/api/"; must start
 * with '/').  Called from a loadable extension's svc_start / svc_stop.
 *   register   -> 0, or -EEXIST (prefix taken) / -ENOMEM (table full) /
 *                 -EINVAL (bad args).
 *   unregister -> stops new dispatches to this prefix, then blocks until any
 *                 in-flight call to its handler returns, so the extension that
 *                 owns it can be unloaded immediately afterwards. */
int  webapi_register(const char *prefix, webapi_handler_fn fn);
void webapi_unregister(const char *prefix);

#endif /* KLAUSSCPU_WEBAPI_H_ */

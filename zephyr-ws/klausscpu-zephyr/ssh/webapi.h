/*
 * webapi.h — HTTP control/telemetry API (/api/*) for the file server.
 *
 *   GET /api/status        -> { uptime_ms, switches, leds, seg, perf{...} }
 *   GET /api/leds?v=<n>     -> set LEDs (decimal or 0x hex), returns { leds }
 *   GET /api/seg?v=<n>      -> set 7-seg, returns { seg }
 *
 * webapi.c (kernel-resident) is only a *dispatcher*: it forwards "/api/..."
 * requests to a backend handler that a loadable service extension
 * (apibackend.llext) installs via webapi_register().  The route logic above
 * lives in that extension, so it can be loaded/unloaded/swapped at runtime
 * without rebuilding the kernel.  When no backend is registered, /api/* -> 503.
 */
#ifndef KLAUSSCPU_WEBAPI_H_
#define KLAUSSCPU_WEBAPI_H_

#include <stdbool.h>

struct httpd_conn;

/* Dispatch an "/api/..." request to the registered backend handler (or send a
 * 503 if none is loaded).  Called by httpd_serve(); returns true when it owns
 * the response, false if url is not an API route (caller falls back to files). */
bool webapi_handle(struct httpd_conn *c, const char *method, const char *url,
		   const char *query);

/* Backend handler signature — same contract as webapi_handle for an /api route:
 * build/send the response and return true (handled). */
typedef bool (*webapi_handler_fn)(struct httpd_conn *c, const char *method,
				  const char *url, const char *query);

/* Install / remove the backend handler (called from a loadable extension's
 * svc_start / svc_stop).  Registering replaces any previous handler.
 * unregister() blocks until no server thread is still executing the handler, so
 * the extension that owns it can be safely unloaded immediately afterwards. */
void webapi_register(webapi_handler_fn fn);
void webapi_unregister(void);

#endif /* KLAUSSCPU_WEBAPI_H_ */

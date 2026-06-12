/*
 * webapi.c — prefix-keyed dispatcher for pluggable HTTP backends.
 *
 * Loadable service extensions register a handler for a URL prefix (e.g. "/api/")
 * via webapi_register(); httpd_serve() (shared by the :80 plain and :443 TLS
 * servers) calls webapi_handle() before file serving, which routes a request to
 * the backend whose prefix is the longest match, or returns false so the file
 * server takes it (404 if no such file).  Several backends coexist, one per
 * prefix, each load/unload-able at runtime.
 *
 * Concurrency: the :80 and :443 server threads may both be inside a handler at
 * once while a `svc stop` runs on a shell thread.  The route table is swapped
 * only under api_lock; a per-route inflight refcount is taken (under the lock)
 * around each handler call, and webapi_unregister() clears the route's fn then
 * drains its inflight to zero before freeing the slot — so a handler's code is
 * guaranteed idle before the extension that owns it is unloaded.  A lockless
 * active-route count short-circuits the common (no-backend) file path.
 */

#include <zephyr/kernel.h>
#include <string.h>
#include <errno.h>
#ifdef CONFIG_LLEXT
#include <zephyr/llext/symbol.h>
#endif

#include "httpd.h"
#include "webapi.h"

#define WEBAPI_MAX_BACKENDS 4

struct webapi_route {
	char              prefix[24];  /* "" == free slot */
	webapi_handler_fn fn;          /* NULL while being torn down */
	atomic_t          inflight;    /* server threads currently in fn */
};

static struct webapi_route routes[WEBAPI_MAX_BACKENDS];
static atomic_t active_routes;     /* lockless fast-path hint */
static K_MUTEX_DEFINE(api_lock);

int webapi_register(const char *prefix, webapi_handler_fn fn)
{
	if (prefix == NULL || prefix[0] != '/' || fn == NULL ||
	    strlen(prefix) >= sizeof(routes[0].prefix)) {
		return -EINVAL;
	}

	k_mutex_lock(&api_lock, K_FOREVER);

	struct webapi_route *slot = NULL;

	for (int i = 0; i < WEBAPI_MAX_BACKENDS; i++) {
		if (routes[i].prefix[0] != '\0') {
			if (strcmp(routes[i].prefix, prefix) == 0) {
				k_mutex_unlock(&api_lock);
				return -EEXIST;
			}
		} else if (slot == NULL) {
			slot = &routes[i];
		}
	}
	if (slot == NULL) {
		k_mutex_unlock(&api_lock);
		return -ENOMEM;
	}

	(void)strncpy(slot->prefix, prefix, sizeof(slot->prefix) - 1);
	slot->prefix[sizeof(slot->prefix) - 1] = '\0';
	atomic_set(&slot->inflight, 0);
	slot->fn = fn;
	atomic_inc(&active_routes);

	k_mutex_unlock(&api_lock);
	return 0;
}

void webapi_unregister(const char *prefix)
{
	if (prefix == NULL) {
		return;
	}

	k_mutex_lock(&api_lock, K_FOREVER);

	struct webapi_route *r = NULL;

	for (int i = 0; i < WEBAPI_MAX_BACKENDS; i++) {
		if (routes[i].prefix[0] != '\0' &&
		    strcmp(routes[i].prefix, prefix) == 0) {
			r = &routes[i];
			break;
		}
	}
	if (r == NULL) {
		k_mutex_unlock(&api_lock);
		return;
	}

	/* Stop new dispatches matching this route, then drop the count so the
	 * file fast-path can skip the table again once this is the last route. */
	r->fn = NULL;
	atomic_dec(&active_routes);
	k_mutex_unlock(&api_lock);

	/* Wait for any in-flight handler call on this route to return before the
	 * caller unloads the extension that owns the handler code. */
	while (atomic_get(&r->inflight) != 0) {
		k_sleep(K_MSEC(5));
	}

	k_mutex_lock(&api_lock, K_FOREVER);
	r->prefix[0] = '\0';
	k_mutex_unlock(&api_lock);
}

#ifdef CONFIG_LLEXT
EXPORT_SYMBOL(webapi_register);
EXPORT_SYMBOL(webapi_unregister);
#endif

bool webapi_handle(struct httpd_conn *c, const char *method, const char *url,
		   const char *query)
{
	/* Common case (no backend loaded): skip the table + lock entirely. */
	if (atomic_get(&active_routes) == 0) {
		return false;
	}

	k_mutex_lock(&api_lock, K_FOREVER);

	struct webapi_route *best = NULL;
	size_t best_len = 0;

	for (int i = 0; i < WEBAPI_MAX_BACKENDS; i++) {
		struct webapi_route *r = &routes[i];

		if (r->fn == NULL || r->prefix[0] == '\0') {
			continue;
		}

		size_t plen = strlen(r->prefix);

		if (plen > best_len && strncmp(url, r->prefix, plen) == 0) {
			best = r;
			best_len = plen;
		}
	}

	webapi_handler_fn fn = NULL;

	if (best != NULL) {
		fn = best->fn;
		atomic_inc(&best->inflight);   /* pin while we call it */
	}
	k_mutex_unlock(&api_lock);

	if (fn == NULL) {
		return false;   /* no backend owns this URL -> file serving */
	}

	bool ret = fn(c, method, url, query);

	atomic_dec(&best->inflight);
	return ret;
}

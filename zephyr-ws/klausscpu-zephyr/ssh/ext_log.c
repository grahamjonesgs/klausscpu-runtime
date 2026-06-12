/*
 * ext_log.c — logging entry point exported to LLEXT extensions.
 *
 * An extension cannot own a Zephyr log module: LOG_MODULE_REGISTER places a
 * struct log_source_const_data into an iterable linker section that the llext
 * loader does not merge into the kernel's, so a module defined inside an
 * extension is never registered with the backends and its LOG_* calls have no
 * live source to attach to.  Instead the kernel registers one "ext" module here
 * on behalf of all extensions and exports ext_log(); extension messages then
 * flow through the normal logging backends — including the SD-card filesystem
 * backend — so they land in /SD:/log alongside the kernel's own logs.
 *
 * The extension pre-formats the message (snprintf/snprintk is already exported
 * in llext_exports.c) and passes the finished string, so no varargs cross the
 * kernel/extension boundary.  See ext_log_compat.h for the LOG_* shim that lets
 * a shared source (e.g. httpd.c) keep its existing LOG_ERR/WRN/INF call sites
 * unchanged when compiled into an extension.
 */

#include <zephyr/logging/log.h>
#include <zephyr/llext/symbol.h>

#include "ext_log.h"

LOG_MODULE_REGISTER(ext, LOG_LEVEL_INF);

void ext_log(int level, const char *msg)
{
	switch (level) {
	case EXT_LOG_ERR:
		LOG_ERR("%s", msg);
		break;
	case EXT_LOG_WRN:
		LOG_WRN("%s", msg);
		break;
	default:
		LOG_INF("%s", msg);
		break;
	}
}
EXPORT_SYMBOL(ext_log);

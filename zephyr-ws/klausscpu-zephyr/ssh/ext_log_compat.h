/*
 * ext_log_compat.h — LOG_* shim for sources compiled INTO an LLEXT extension.
 *
 * A shared source such as httpd.c uses Zephyr's LOG_MODULE_REGISTER + LOG_* in
 * the kernel build, but neither works inside an extension (the module can't be
 * registered — see ext_log.c).  Including this header in the extension build,
 * in place of <zephyr/logging/log.h>, keeps those call sites compiling and
 * routes them to the kernel's exported ext_log(): the message is formatted here
 * with snprintk and handed over as a finished string.
 *
 * Wiring — guard the include in the shared source so the kernel build is
 * unaffected (LL_EXTENSION_BUILD is defined automatically by add_llext_target):
 *
 *     #ifdef LL_EXTENSION_BUILD
 *     #include "ext_log_compat.h"
 *     #else
 *     #include <zephyr/logging/log.h>
 *     #endif
 *     ...
 *     LOG_MODULE_REGISTER(httpd, LOG_LEVEL_INF);   // becomes a no-op here
 *
 * <zephyr/logging/log.h> is pulled in transitively anyway (via <zephyr/kernel.h>),
 * so this header is in practice always processed AFTER log.h — hence it must
 * #undef each macro before redefining it (otherwise log.h's versions win and we
 * get "macro redefined" warnings).
 */
#ifndef KLAUSSCPU_EXT_LOG_COMPAT_H_
#define KLAUSSCPU_EXT_LOG_COMPAT_H_

#include <zephyr/sys/cbprintf.h>   /* snprintk */

#include "ext_log.h"

#define EXT_LOG_AT(_lvl, ...)                                   \
	do {                                                    \
		char _ext_lb[160];                              \
		(void)snprintk(_ext_lb, sizeof(_ext_lb),        \
			       __VA_ARGS__);                    \
		ext_log((_lvl), _ext_lb);                       \
	} while (0)

/* Override log.h's macros (already defined via the transitive include). */
#undef LOG_MODULE_REGISTER
#undef LOG_MODULE_DECLARE
#undef LOG_ERR
#undef LOG_WRN
#undef LOG_INF
#undef LOG_DBG

/* No per-extension module — the kernel owns the "ext" module (ext_log.c). */
#define LOG_MODULE_REGISTER(...)
#define LOG_MODULE_DECLARE(...)

#define LOG_ERR(...) EXT_LOG_AT(EXT_LOG_ERR, __VA_ARGS__)
#define LOG_WRN(...) EXT_LOG_AT(EXT_LOG_WRN, __VA_ARGS__)
#define LOG_INF(...) EXT_LOG_AT(EXT_LOG_INF, __VA_ARGS__)
#define LOG_DBG(...) EXT_LOG_AT(EXT_LOG_INF, __VA_ARGS__)

#endif /* KLAUSSCPU_EXT_LOG_COMPAT_H_ */

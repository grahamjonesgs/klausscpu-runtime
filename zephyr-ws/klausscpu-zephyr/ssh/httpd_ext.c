/*
 * httpd_ext.c — the plain HTTP file server (httpd.c) + JSON API (webapi.c)
 * packaged as a loadable, detachable LLEXT service.
 *
 * CONFIG_LLEXT_TYPE_ELF_OBJECT (this app's llext type) builds an extension from
 * a SINGLE source file, so this is a unity translation unit that #includes the
 * two shared sources verbatim — keeping httpd.c / webapi.c as the one source of
 * truth shared with the kernel-resident build.  Compiled by add_llext_target()
 * in apps/ssh_shell/CMakeLists.txt with LL_EXTENSION_BUILD defined, so httpd.c
 * pulls in ext_log_compat.h (LOG_* -> ext_log) instead of the logging subsystem.
 *
 * The service entry points the loader resolves (see llext_loader.c):
 *   svc_start() — spawn the httpd worker thread and return immediately (detach)
 *   svc_stop()  — stop the worker and join it before the loader unloads us
 * Both must be GLOBAL so they land in the extension's symbol table.
 *
 * All kernel symbols these reference resolve at load time: the fs_ API, snprintk
 * and the str helpers via ssh/llext_exports.c, and the k_ and zsock_ syscalls
 * via Zephyr's built-in subsys/llext/llext_export.c.
 */

#include "httpd.c"
#include "webapi.c"

int svc_start(void)
{
	httpd_start();
	return 0;
}

int svc_stop(void)
{
	httpd_stop();
	return 0;
}

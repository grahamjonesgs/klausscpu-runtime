/*
 * llext_exports.c — kernel symbols exported to loadable extensions.
 *
 * Extensions (former PIC demo programs, now built as ELFCLASS32 ET_REL
 * objects) call the standard C library directly; the llext loader resolves
 * those UND references against this table.  The functions themselves are the
 * kernel's own minimal/common libc, so a single libc is shared between the
 * kernel and every extension.
 *
 * Console routing: printf/puts/putchar funnel through arch_printk_char_out(),
 * which the run loader redirects to the SSH session.  getchar() has no
 * minimal-libc implementation, so it is provided here and reads through the
 * input-redirect hook the loader installs.
 */

#include <zephyr/llext/symbol.h>
#include <zephyr/sys/printk.h>       /* snprintk */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef CONFIG_FILE_SYSTEM
#include <zephyr/fs/fs.h>
#endif

/*
 * Minimal libc's <stdio.h> macro-defines putchar(c) as putc(c, stdout); undo
 * that so EXPORT_SYMBOL(putchar) refers to the real function defined in
 * lib/libc/minimal/source/stdout/stdout_console.c.
 */
#undef putchar
extern int putchar(int c);

/*
 * Console input redirect, mirror of klausscpu_console_out_hook (in
 * arch/klausscpu/core/irq.c).  NULL when no extension is running; otherwise
 * set by the run loader to pull bytes from the active SSH session.
 */
int (*klausscpu_console_in_hook)(void);

int getchar(void)
{
	int (*hook)(void) = klausscpu_console_in_hook;

	if (hook != NULL) {
		return hook();
	}

	/* No bare-console input path is wired in this build. */
	return EOF;
}

/* stdio */
EXPORT_SYMBOL(printf);
EXPORT_SYMBOL(snprintf);
EXPORT_SYMBOL(puts);
EXPORT_SYMBOL(putchar);
EXPORT_SYMBOL(getchar);

/* stdlib — malloc family (CONFIG_COMMON_LIBC_MALLOC) */
EXPORT_SYMBOL(malloc);
EXPORT_SYMBOL(calloc);
EXPORT_SYMBOL(realloc);
EXPORT_SYMBOL(free);

/* string / memory */
EXPORT_SYMBOL(memcpy);
EXPORT_SYMBOL(memmove);
EXPORT_SYMBOL(memset);
EXPORT_SYMBOL(memcmp);
EXPORT_SYMBOL(strlen);
EXPORT_SYMBOL(strcmp);
EXPORT_SYMBOL(strncmp);
EXPORT_SYMBOL(strcpy);
EXPORT_SYMBOL(strncpy);
EXPORT_SYMBOL(strcat);
EXPORT_SYMBOL(strchr);
EXPORT_SYMBOL(strrchr);
EXPORT_SYMBOL(strstr);

/* stdlib — numeric parse (httpd URL/query handling) */
EXPORT_SYMBOL(strtol);
EXPORT_SYMBOL(strtoul);

/* Zephyr formatted output (httpd builds responses with snprintk). */
EXPORT_SYMBOL(snprintk);

/* Filesystem API for extensions that serve files (the loadable HTTP server in
 * httpd.c/webapi.c).
 *
 * NOTE: kernel threading/uptime/errno and the whole socket (zsock) syscall API
 * are ALREADY exported by Zephyr's built-in subsys/llext/llext_export.c (it
 * covers every k_ and zsock syscall, the mem/str basics, uart, net_if, log,
 * etc.), so we must NOT re-export those - only symbols Zephyr's table omits.
 * The fs API is one such gap.  Guarded so llext builds without a filesystem
 * (e.g. the llext_demo app, which also pulls in this file) still link. */
#ifdef CONFIG_FILE_SYSTEM
EXPORT_SYMBOL(fs_open);
EXPORT_SYMBOL(fs_close);
EXPORT_SYMBOL(fs_read);
EXPORT_SYMBOL(fs_write);
EXPORT_SYMBOL(fs_opendir);
EXPORT_SYMBOL(fs_readdir);
EXPORT_SYMBOL(fs_closedir);
EXPORT_SYMBOL(fs_stat);
#endif /* CONFIG_FILE_SYSTEM */

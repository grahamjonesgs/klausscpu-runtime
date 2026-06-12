/* llext_loader.h — Load and run an LLEXT extension from SD card. */

#ifndef LLEXT_LOADER_H
#define LLEXT_LOADER_H

#include <zephyr/shell/shell.h>

void llext_loader_init(void);

/*
 * llext_run_from_sd() — load an ELFCLASS32 ET_REL extension from SD, resolve
 *                       its `main`, run it on the calling thread, block until it
 *                       returns, and return its exit code.
 *
 * The extension's stdout (printf/puts/putchar) and stdin (getchar) are
 * redirected to `sh` for the duration of the run.  For an SSH shell, stdout
 * goes to the wolfSSH session and stdin is drained from that session's RX ring
 * (ssh_shell_getc); for the serial console (or any non-SSH shell) stdout falls
 * through to the UART and stdin returns EOF.
 *
 * Returns:
 *   >= 0  : extension main() return value
 *    -1   : file open/read failed
 *    -2   : llext_load failed
 *    -3   : `main` not found in the extension
 *    -4   : out of memory / thread resource failure
 */
int llext_run_from_sd(const char *filename, const struct shell *sh);

/*
 * Resident background services
 * ----------------------------
 * Unlike llext_run_from_sd() (which runs an extension's main() to completion on
 * the calling thread and then unloads it), a service extension stays loaded and
 * runs detached on its own thread(s).  Such an extension exports two GLOBAL
 * entry points:
 *
 *   int svc_start(void);   spawn the worker thread(s) and return promptly (0 = ok)
 *   int svc_stop(void);    stop AND k_thread_join the worker(s), then return
 *
 * svc_stop() MUST join its threads before returning: llext_service_stop()
 * unloads the image (freeing the threads' stacks) as soon as svc_stop()
 * returns.
 */

/* Load `filename` from SD as a resident service registered under `name`
 * (<=15 chars, unique), resolve and call its svc_start(), and keep it loaded.
 * Returns 0 on success; negative errno on failure (image freed on any error). */
int llext_service_load(const char *filename, const char *name);

/* Stop and unload the resident service `name`: call its svc_stop() (joins the
 * worker), then unload + free.  Returns 0, or -ENOENT if no such service. */
int llext_service_stop(const char *name);

/* Print the resident-service table (one name per line).  Routes to `sh` if
 * non-NULL (so it reaches an SSH session), else to printk (UART/log). */
void llext_service_list(const struct shell *sh);

#endif /* LLEXT_LOADER_H */

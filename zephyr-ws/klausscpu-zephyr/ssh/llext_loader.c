/* llext_loader.c — Load and run an LLEXT extension from the SD card,
 *                  redirecting the extension's stdio to an SSH session.
 *
 * Replaces the custom PIC loader (pic_loader.c).  Extensions are ELFCLASS32
 * ET_REL objects produced by the KlaussCPU clang with `-c`; they call the
 * kernel's exported libc (see llext_exports.c) rather than bundling their own.
 *
 * Concurrency: the extension's `main` runs ON THE CALLING (per-connection) SSH
 * thread, and stdio is routed by the *currently running thread* via thread
 * custom data.  Because every SSH session already has its own connection thread
 * (conn_threads[SSH_MAX_CONNS]), several extensions run concurrently, each with
 * its stdout/stdin bound to its own session.  llext_load()/llext_unload() are
 * internally serialised by the subsystem's llext_lock, so concurrent loads are
 * safe; nothing here holds a global lock across a run.
 */

#define WOLFSSL_USER_SETTINGS
#include "user_settings.h"

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/llext/llext.h>
#include <zephyr/llext/buf_loader.h>
#include <zephyr/logging/log.h>

#include <errno.h>

#include <stdio.h>
#include <string.h>

#include <wolfssh/ssh.h>

#include "llext_loader.h"
#include "ssh_shell_transport.h"

LOG_MODULE_REGISTER(llext_loader, LOG_LEVEL_INF);

#define EXT_MAX_SIZE    (8UL * 1024 * 1024)

/* Console redirect hooks (declared in arch/klausscpu/core/irq.c and
 * ssh/llext_exports.c).  Set once in llext_loader_init(); each callback then
 * keys off the running thread, so they need no per-run mutation. */
extern int (*klausscpu_console_out_hook)(int c);
extern int (*klausscpu_console_in_hook)(void);

typedef int (*ext_entry_fn)(int argc, char **argv);

/* Per-run I/O context, pointed to by the running thread's custom data while its
 * main() executes.  Lives on that thread's stack for the duration of the run. */
struct ext_ctx {
	const struct shell *sh;   /* owning shell (for input routing)   */
	WOLFSSH            *ssh;   /* its SSH session, or NULL (serial)  */
};

/* ── Console routing (run on the extension's own thread) ──────────────────── */

/* Output router for arch_printk_char_out(): returns 1 if it sent the char to
 * the current thread's SSH session, 0 to let it fall through to the UART. */
static int route_out(int c)
{
	struct ext_ctx *ctx = k_thread_custom_data_get();

	if (ctx == NULL || ctx->ssh == NULL) {
		return 0;
	}

	WOLFSSH *ssh = ctx->ssh;

	if (c == '\n') {
		uint8_t cr = '\r';

		wolfSSH_stream_send(ssh, &cr, 1);
	}

	uint8_t ch = (uint8_t)c;

	wolfSSH_stream_send(ssh, &ch, 1);
	return 1;
}

/* Input source for the exported getchar(): drains one byte from the owning
 * shell's RX ring (ssh_shell_getc), so the socket is never read here AND on the
 * connection thread at the same time.  Returns -1 (EOF) when this thread is not
 * running an extension, on a non-SSH shell, or when the session closes. */
static int route_in(void)
{
	struct ext_ctx *ctx = k_thread_custom_data_get();

	if (ctx == NULL) {
		return -1;
	}
	return ssh_shell_getc(ctx->sh);
}

/* ── Status output helper — SSH or printk ─────────────────────────────────── */

static void loader_printf(WOLFSSH *ssh, const char *fmt, ...)
{
	char buf[160];
	va_list ap;

	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);

	va_end(ap);
	if (n > 0) {
		if (ssh != NULL) {
			wolfSSH_stream_send(ssh, (uint8_t *)buf, (word32)n);
		} else {
			printk("%s", buf);
		}
	}
}

/* ── Read the whole extension file into a RAM buffer ──────────────────────── */

static int read_file(const char *path, WOLFSSH *ssh, uint8_t **buf_out,
		     size_t *size_out)
{
	struct fs_dirent stat;

	if (fs_stat(path, &stat) != 0) {
		loader_printf(ssh, "Cannot stat '%s'\r\n", path);
		return -1;
	}

	size_t fsize = stat.size;

	if (fsize == 0 || fsize > EXT_MAX_SIZE) {
		loader_printf(ssh, "File size out of range (%lu)\r\n",
			      (unsigned long)fsize);
		return -1;
	}

	uint8_t *buf = k_malloc(fsize);

	if (buf == NULL) {
		loader_printf(ssh, "Out of memory for %lu-byte image\r\n",
			      (unsigned long)fsize);
		return -1;
	}

	struct fs_file_t f;

	fs_file_t_init(&f);
	if (fs_open(&f, path, FS_O_READ) != 0) {
		loader_printf(ssh, "Cannot open '%s'\r\n", path);
		k_free(buf);
		return -1;
	}

	ssize_t got = fs_read(&f, buf, fsize);

	fs_close(&f);

	if (got < 0 || (size_t)got != fsize) {
		loader_printf(ssh, "Read failed\r\n");
		k_free(buf);
		return -1;
	}

	*buf_out = buf;
	*size_out = fsize;
	return 0;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void llext_loader_init(void)
{
	/* Install the per-thread console routers once; they are no-ops on
	 * threads that are not currently running an extension. */
	klausscpu_console_out_hook = route_out;
	klausscpu_console_in_hook = route_in;
}

int llext_run_from_sd(const char *filename, const struct shell *sh)
{
	WOLFSSH *ssh = ssh_shell_ssh(sh);   /* NULL on the serial console */
	uint8_t *buf = NULL;
	size_t size = 0;

	if (read_file(filename, ssh, &buf, &size) != 0) {
		return -1;
	}

	loader_printf(ssh, "'%s'  %lu bytes\r\n", filename, (unsigned long)size);

	struct llext_buf_loader buf_loader = LLEXT_BUF_LOADER(buf, size);
	struct llext_loader *ldr = &buf_loader.loader;
	struct llext_load_param ldr_parm = LLEXT_LOAD_PARAM_DEFAULT;
	struct llext *ext = NULL;

	/* Keep ext->sym_tab after load so we can resolve `main` by name. */
	ldr_parm.keep_symtab = true;

	/* llext_load() deduplicates by name (a second load of the same name just
	 * bumps a use count and returns the existing extension).  We always want a
	 * fresh, independent instance — even of the same program in another session
	 * — so give every load a unique name. */
	static atomic_t ext_seq;
	char ext_name[16];

	snprintf(ext_name, sizeof(ext_name), "ext%u",
		 (unsigned int)atomic_inc(&ext_seq));

	int ret = llext_load(ldr, ext_name, &ext, &ldr_parm);

	if (ret != 0) {
		loader_printf(ssh, "llext_load failed: %d\r\n", ret);
		k_free(buf);
		return -2;
	}

	ext_entry_fn entry =
		(ext_entry_fn)llext_find_sym(&ext->sym_tab, "main");

	if (entry == NULL) {
		loader_printf(ssh, "No 'main' symbol in extension\r\n");
		llext_unload(&ext);
		k_free(buf);
		return -3;
	}

	loader_printf(ssh, "Running main @ %p ...\r\n", (void *)entry);

	/* Route this thread's stdout/stdin to the SSH session while main() runs,
	 * then restore.  Running on the connection thread (rather than a shared
	 * worker) is what lets multiple sessions run extensions concurrently. */
	struct ext_ctx ctx = { .sh = sh, .ssh = ssh };
	void *prev = k_thread_custom_data_get();

	k_thread_custom_data_set(&ctx);
	int result = entry(0, NULL);

	k_thread_custom_data_set(prev);

	llext_unload(&ext);
	k_free(buf);

	return result;
}

/* ── Resident background services (svc_start/svc_stop extensions) ─────────── */

#define EXT_SVC_MAX 4

typedef int (*svc_fn)(void);

/* One loaded, running service.  `ext`/`buf` are kept alive for the whole run
 * (the worker thread's stack lives in the extension image); both are released
 * only after svc_stop() has joined that thread. */
struct ext_service {
	char          name[16];
	struct llext *ext;
	uint8_t      *buf;
	svc_fn        stop;
};

static struct ext_service svc_tab[EXT_SVC_MAX];
static K_MUTEX_DEFINE(svc_lock);

int llext_service_load(const char *filename, const char *name)
{
	if (name == NULL || name[0] == '\0' ||
	    strlen(name) >= sizeof(svc_tab[0].name)) {
		return -EINVAL;
	}

	k_mutex_lock(&svc_lock, K_FOREVER);

	/* Reject a duplicate name and find a free slot in one pass. */
	struct ext_service *slot = NULL;

	for (int i = 0; i < EXT_SVC_MAX; i++) {
		if (svc_tab[i].ext != NULL &&
		    strcmp(svc_tab[i].name, name) == 0) {
			k_mutex_unlock(&svc_lock);
			LOG_ERR("service '%s' already loaded", name);
			return -EEXIST;
		}
		if (svc_tab[i].ext == NULL && slot == NULL) {
			slot = &svc_tab[i];
		}
	}
	if (slot == NULL) {
		k_mutex_unlock(&svc_lock);
		LOG_ERR("service table full (%d)", EXT_SVC_MAX);
		return -ENOMEM;
	}

	uint8_t *buf = NULL;
	size_t size = 0;

	if (read_file(filename, NULL, &buf, &size) != 0) {
		k_mutex_unlock(&svc_lock);
		return -EIO;
	}

	struct llext_buf_loader buf_loader = LLEXT_BUF_LOADER(buf, size);
	struct llext_load_param ldr_parm = LLEXT_LOAD_PARAM_DEFAULT;
	struct llext *ext = NULL;

	/* keep_symtab so svc_start/svc_stop resolve by name (as llext_run_*). */
	ldr_parm.keep_symtab = true;

	int ret = llext_load(&buf_loader.loader, name, &ext, &ldr_parm);

	if (ret != 0) {
		k_mutex_unlock(&svc_lock);
		LOG_ERR("service '%s' llext_load failed: %d", name, ret);
		k_free(buf);
		return -ENOEXEC;
	}

	svc_fn start = (svc_fn)llext_find_sym(&ext->sym_tab, "svc_start");
	svc_fn stop  = (svc_fn)llext_find_sym(&ext->sym_tab, "svc_stop");

	if (start == NULL || stop == NULL) {
		k_mutex_unlock(&svc_lock);
		LOG_ERR("service '%s' missing svc_start/svc_stop", name);
		llext_unload(&ext);
		k_free(buf);
		return -ENOSYS;
	}

	/* svc_start() spawns the worker and returns promptly; held under
	 * svc_lock, which is fine as long as it doesn't block. */
	int rc = start();

	if (rc != 0) {
		k_mutex_unlock(&svc_lock);
		LOG_ERR("service '%s' svc_start failed: %d", name, rc);
		llext_unload(&ext);
		k_free(buf);
		return -EAGAIN;
	}

	strncpy(slot->name, name, sizeof(slot->name) - 1);
	slot->name[sizeof(slot->name) - 1] = '\0';
	slot->ext = ext;
	slot->buf = buf;
	slot->stop = stop;

	k_mutex_unlock(&svc_lock);
	LOG_INF("service '%s' started (%zu bytes)", name, size);
	return 0;
}

int llext_service_stop(const char *name)
{
	k_mutex_lock(&svc_lock, K_FOREVER);

	struct ext_service *slot = NULL;

	for (int i = 0; i < EXT_SVC_MAX; i++) {
		if (svc_tab[i].ext != NULL &&
		    strcmp(svc_tab[i].name, name) == 0) {
			slot = &svc_tab[i];
			break;
		}
	}
	if (slot == NULL) {
		k_mutex_unlock(&svc_lock);
		return -ENOENT;
	}

	/* svc_stop() must stop and JOIN the worker before returning; only then
	 * is it safe to unload (which frees that thread's stack). */
	int rc = slot->stop();
	struct llext *ext = slot->ext;
	uint8_t *buf = slot->buf;

	slot->ext = NULL;
	slot->buf = NULL;
	slot->stop = NULL;
	slot->name[0] = '\0';

	k_mutex_unlock(&svc_lock);

	if (rc != 0) {
		LOG_WRN("service '%s' svc_stop returned %d", name, rc);
	}
	llext_unload(&ext);
	k_free(buf);
	LOG_INF("service '%s' stopped", name);
	return 0;
}

void llext_service_list(const struct shell *sh)
{
	k_mutex_lock(&svc_lock, K_FOREVER);

	int n = 0;

	for (int i = 0; i < EXT_SVC_MAX; i++) {
		if (svc_tab[i].ext != NULL) {
			if (sh != NULL) {
				shell_print(sh, "  %s", svc_tab[i].name);
			} else {
				printk("  %s\n", svc_tab[i].name);
			}
			n++;
		}
	}
	if (n == 0) {
		if (sh != NULL) {
			shell_print(sh, "  (no services loaded)");
		} else {
			printk("  (no services loaded)\n");
		}
	}
	k_mutex_unlock(&svc_lock);
}

/* ssh_shell_transport.c — Zephyr shell transport backend over wolfSSH.
 *
 * Replaces the hand-rolled interpreter that used to live in ssh_server.c
 * (run_shell + dispatch_command + a private copy of every cmd_*).  SSH is now
 * just another shell transport, so the single set of SHELL_CMD_REGISTER
 * commands (shell_cmds.c) is shared between the serial console and SSH, and
 * line editing / history / tab-completion / `help` all come from the core.
 *
 * One static shell instance is defined per connection slot (Zephyr shells are
 * static — cf. the upstream telnet backend, which supports only one session).
 *
 * ── Concurrency ──────────────────────────────────────────────────────────────
 * Two threads touch one WOLFSSH session:
 *   reader  = the per-connection thread (ssh_server.c) — the ONLY caller of
 *             wolfSSH_stream_read.  It feeds bytes into rx_rb (ssh_shell_feed).
 *   writers = the shell core thread (transport write(), ssh_shell_write) and
 *             a `run` extension's thread (ssh_shell_send).
 * The read path SENDS too: wolfSSH_stream_read() answers with a channel
 * WINDOW_ADJUST once half the receive window is used, replies to the peer's
 * keepalive requests and runs a peer-initiated rekey.  wolfSSH keeps one
 * output buffer and one sequence number per session, so every wolfSSH call is
 * made under the slot's io_lock, the reader's included; the reader drops the
 * lock only inside the socket recv callback while it waits for bytes
 * (ssh_server.c ssh_recv), where wolfSSH is not mid-send.  (Before this, a
 * window adjust racing with output corrupted the stream every few hundred KB
 * of input and the client dropped the session.)  Input for a running `run`
 * extension is taken from the SAME rx_rb (ssh_shell_getc), so the socket is
 * never read from two threads simultaneously.
 */

#define WOLFSSL_USER_SETTINGS
#include "user_settings.h"

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <wolfssh/ssh.h>

#include "ssh_server.h"          /* SSH_MAX_CONNS */
#include "ssh_shell_transport.h"
#include "llext_loader.h"
#if defined(CONFIG_KLAUSSCPU_SHELL_CMDS)
#include "shell_paths.h"
#endif

LOG_MODULE_REGISTER(ssh_shell_transport, LOG_LEVEL_INF);

/* Input queued for the shell / a running program.  The reader thread doesn't
 * block when it is full (it must keep reading for wolfSSH's window updates,
 * which the writer may be waiting on), so it is sized for a pasted block:
 * a terminal paste arrives as one SSH packet. */
#define RX_RING_SIZE 4096

/* ── Per-slot transport context ──────────────────────────────────────────── */

struct ssh_shell_ctx {
	WOLFSSH                   *ssh;      /* bound session, NULL when idle  */
	shell_transport_handler_t  handler;  /* shell core's event callback    */
	void                      *context;
	struct ring_buf            rx_rb;
	uint8_t                    rx_buf[RX_RING_SIZE];
	struct k_sem               rx_sem;   /* wakeup for ssh_shell_getc()    */
	struct k_mutex             io_lock;  /* serialises every wolfSSH call   */
	atomic_t                   open;
	atomic_t                   running;  /* `run`/`python` is executing     */
	struct k_sem               done;     /* given when that program ends    */
	ssh_shell_rx_hook_t        rx_hook;  /* sees input first while set      */
};

/* ── Transport API ───────────────────────────────────────────────────────── */

static int ctx_write(struct ssh_shell_ctx *c, const void *data, size_t len);

static int t_init(const struct shell_transport *transport, const void *config,
		  shell_transport_handler_t evt_handler, void *context)
{
	struct ssh_shell_ctx *c = transport->ctx;

	ARG_UNUSED(config);
	c->handler = evt_handler;
	c->context = context;
	ring_buf_init(&c->rx_rb, sizeof(c->rx_buf), c->rx_buf);
	k_sem_init(&c->rx_sem, 0, 1);
	k_sem_init(&c->done, 0, 1);
	k_mutex_init(&c->io_lock);
	atomic_set(&c->open, 0);
	atomic_set(&c->running, 0);
	return 0;
}

static int t_uninit(const struct shell_transport *transport)
{
	ARG_UNUSED(transport);
	return 0;
}

static int t_enable(const struct shell_transport *transport, bool blocking)
{
	ARG_UNUSED(transport);
	ARG_UNUSED(blocking);
	return 0;
}

static int t_write(const struct shell_transport *transport, const void *data,
		   size_t length, size_t *cnt)
{
	struct ssh_shell_ctx *c = transport->ctx;

	/* No bound session (idle, or already released): swallow output. */
	if (c->ssh == NULL || !atomic_get(&c->open)) {
		*cnt = length;
		return 0;
	}

	/* All of it, waiting out a full window or a rekey: the shell core
	 * drops the rest of a message on an error. */
	int n = ctx_write(c, data, length);

	if (n < 0) {
		*cnt = 0;
		return n;
	}
	*cnt = length;
	return 0;
}

static int t_read(const struct shell_transport *transport, void *data,
		  size_t length, size_t *cnt)
{
	struct ssh_shell_ctx *c = transport->ctx;

	*cnt = ring_buf_get(&c->rx_rb, data, length);
	return 0;
}

static const struct shell_transport_api ssh_shell_api = {
	.init   = t_init,
	.uninit = t_uninit,
	.enable = t_enable,
	.write  = t_write,
	.read   = t_read,
};

/* ── One static shell instance per connection slot ───────────────────────── */

#define DEFINE_SSH_SHELL(i, _)                                                 \
	static struct ssh_shell_ctx ssh_ctx_##i;                               \
	static struct shell_transport ssh_tr_##i = {                           \
		.api = &ssh_shell_api,                                         \
		.ctx = &ssh_ctx_##i,                                           \
	};                                                                     \
	SHELL_DEFINE(ssh_shell_##i, "$ ", &ssh_tr_##i, 4, 0,                   \
		     SHELL_FLAG_OLF_CRLF);

LISTIFY(SSH_MAX_CONNS, DEFINE_SSH_SHELL, ())

#define SH_PTR(i, _)  &ssh_shell_##i
#define CTX_PTR(i, _) &ssh_ctx_##i

static const struct shell *const ssh_shells[SSH_MAX_CONNS] = {
	LISTIFY(SSH_MAX_CONNS, SH_PTR, (,))
};
static struct ssh_shell_ctx *const ssh_ctxs[SSH_MAX_CONNS] = {
	LISTIFY(SSH_MAX_CONNS, CTX_PTR, (,))
};

static struct ssh_shell_ctx *ctx_for(const struct shell *sh)
{
	for (int i = 0; i < SSH_MAX_CONNS; i++) {
		if (ssh_shells[i] == sh) {
			return ssh_ctxs[i];
		}
	}
	return NULL;   /* not an SSH shell (e.g. the serial console) */
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void ssh_shell_transport_init(void)
{
	struct shell_backend_config_flags cfg = SHELL_DEFAULT_BACKEND_CONFIG_FLAGS;

	for (int i = 0; i < SSH_MAX_CONNS; i++) {
		/* shell_init() starts the per-instance thread; park it with
		 * shell_stop() until a session claims the slot.  Output is
		 * swallowed by t_write() while ssh == NULL. */
		shell_init(ssh_shells[i], NULL, cfg, false, 0);
		shell_stop(ssh_shells[i]);
	}
}

const struct shell *ssh_shell_claim(int slot, WOLFSSH *ssh)
{
	struct ssh_shell_ctx *c = ssh_ctxs[slot];

	ring_buf_reset(&c->rx_rb);
	k_sem_reset(&c->rx_sem);
	k_sem_reset(&c->done);
	atomic_set(&c->running, 0);
	c->rx_hook = NULL;
	c->ssh = ssh;
	atomic_set(&c->open, 1);
#if defined(CONFIG_KLAUSSCPU_SHELL_CMDS)
	shell_cwd_reset(ssh_shells[slot]);   /* new session starts at /SD:/ */
	shell_prompt_sync(ssh_shells[slot]); /* prompt shows cwd from 1st line */
#endif
	/* The first claim of a slot finds its shell already ACTIVE:
	 * ssh_shell_transport_init()'s shell_stop() ran before the shell thread
	 * did, and the thread then started itself (CONFIG_SHELL_AUTOSTART),
	 * printing its prompt to no session.  shell_start() then refuses
	 * (-ENOTSUP), and the session got no banner or prompt until the user
	 * typed.  Stop it and start again. */
	if (shell_start(ssh_shells[slot]) == -ENOTSUP) {
		(void)shell_stop(ssh_shells[slot]);
		(void)shell_start(ssh_shells[slot]);
	}

	/* shell_start() queues the prompt into the shell's transport buffer but
	 * does not flush it — that only happens at the end of shell_process(),
	 * which the shell thread runs when it wakes on an RX_RDY signal (it
	 * otherwise blocks in k_poll forever).  Without a nudge the first prompt
	 * stays buffered until the user's first keypress.  Raise RX_RDY now (the
	 * ring is empty, so the read is a harmless no-op) to flush it. */
	if (c->handler != NULL) {
		c->handler(SHELL_TRANSPORT_EVT_RX_RDY, c->context);
	}
	return ssh_shells[slot];
}

bool ssh_shell_release(int slot)
{
	struct ssh_shell_ctx *c = ssh_ctxs[slot];
	bool drained = true;

	atomic_set(&c->open, 0);
	k_sem_give(&c->rx_sem);          /* unblock any getc() in `run` -> EOF */

	ssh_shell_rx_hook_t hook = c->rx_hook;

	if (hook != NULL) {
		hook(-1);                /* e.g. interrupt running Python code */
	}

	/* If a `run` extension is still executing on the shell thread, wait for
	 * it to unwind (it should exit promptly once getchar() returns EOF)
	 * before the caller frees the session.  A program that ignores EOF and
	 * loops forever cannot be safely torn down — report that. */
	if (atomic_get(&c->running)) {
		drained = (k_sem_take(&c->done, K_SECONDS(2)) == 0);
	}

	/* -EBUSY: a command still holds the shell (a program that ignored EOF
	 * and didn't finish in time): the session can't be freed. */
	if (shell_stop(ssh_shells[slot]) == -EBUSY) {
		drained = false;
	}

	/* Under io_lock, so a ssh_shell_write() in progress finishes with the
	 * session before it goes. */
	k_mutex_lock(&c->io_lock, K_FOREVER);
	c->ssh = NULL;
	k_mutex_unlock(&c->io_lock);
	return drained;
}

void ssh_shell_feed(int slot, const uint8_t *data, size_t len)
{
	struct ssh_shell_ctx *c = ssh_ctxs[slot];
	ssh_shell_rx_hook_t hook = c->rx_hook;

	size_t lost = 0;

	if (hook == NULL) {
		lost = len - ring_buf_put(&c->rx_rb, data, len);
	} else {
		/* Every byte goes past the hook, even when the ring is full, so
		 * a Ctrl-C behind a paste still interrupts. */
		for (size_t i = 0; i < len; i++) {
			if (!hook(data[i])) {
				lost += 1 - ring_buf_put(&c->rx_rb, &data[i], 1);
			}
		}
	}
	if (lost > 0) {
		LOG_WRN("slot %d: input ring full, %zu bytes dropped", slot, lost);
	}
	k_sem_give(&c->rx_sem);
	if (c->handler != NULL) {
		c->handler(SHELL_TRANSPORT_EVT_RX_RDY, c->context);
	}
}

WOLFSSH *ssh_shell_ssh(const struct shell *sh)
{
	struct ssh_shell_ctx *c = ctx_for(sh);

	return c ? c->ssh : NULL;
}

void ssh_shell_set_rx_hook(const struct shell *sh, ssh_shell_rx_hook_t hook)
{
	struct ssh_shell_ctx *c = ctx_for(sh);

	if (c != NULL) {
		c->rx_hook = hook;
	}
}

struct k_mutex *ssh_shell_io_lock(int slot)
{
	return &ssh_ctxs[slot]->io_lock;
}

static int ctx_write(struct ssh_shell_ctx *c, const void *data, size_t len)
{
	/* wolfSSH_stream_send() sends at most a window/packet's worth and
	 * returns WS_WINDOW_FULL (or WANT_WRITE, REKEYING) when it can't send
	 * now; the reader thread processes the peer's window updates, so wait
	 * and retry.  Give up after 10 s without progress (peer not reading). */
	const uint8_t *p = data;
	size_t left = len;
	int64_t stalled_since = k_uptime_get();

	while (left > 0) {
		k_mutex_lock(&c->io_lock, K_FOREVER);
		if (c->ssh == NULL || !atomic_get(&c->open)) {
			k_mutex_unlock(&c->io_lock);
			return (int)len;      /* session gone: drop the output */
		}
		int n = wolfSSH_stream_send(c->ssh, (uint8_t *)p, (word32)left);
		int err = (n == WS_ERROR) ? wolfSSH_get_error(c->ssh) : n;

		k_mutex_unlock(&c->io_lock);

		if (n > 0) {
			p += n;
			left -= (size_t)n;
			stalled_since = k_uptime_get();
			continue;
		}
		if (err != WS_WINDOW_FULL && err != WS_WANT_WRITE &&
		    err != WS_REKEYING && n != 0) {
			return -EIO;
		}
		if (k_uptime_get() - stalled_since > 10000) {
			return -ETIMEDOUT;
		}
		k_msleep(5);
	}
	return (int)len;
}

int ssh_shell_write(const struct shell *sh, const void *data, size_t len)
{
	struct ssh_shell_ctx *c = ctx_for(sh);

	if (c == NULL) {
		return -ENOTSUP;
	}
	return ctx_write(c, data, len);
}

int ssh_shell_send(WOLFSSH *ssh, const void *data, size_t len)
{
	if (ssh == NULL) {
		return -EINVAL;
	}
	for (int i = 0; i < SSH_MAX_CONNS; i++) {
		if (ssh_ctxs[i]->ssh == ssh) {
			return ctx_write(ssh_ctxs[i], data, len);
		}
	}
	return (int)len;   /* session gone: drop the output */
}

void ssh_shell_run_begin(const struct shell *sh)
{
	struct ssh_shell_ctx *c = ctx_for(sh);

	if (c != NULL) {
		k_sem_reset(&c->done);   /* no stale count from a previous run */
		atomic_set(&c->running, 1);
	}
}

void ssh_shell_run_end(const struct shell *sh)
{
	struct ssh_shell_ctx *c = ctx_for(sh);

	if (c != NULL) {
		atomic_set(&c->running, 0);
		k_sem_give(&c->done);
	}
}

int ssh_shell_getc_wait(const struct shell *sh, k_timeout_t timeout)
{
	struct ssh_shell_ctx *c = ctx_for(sh);
	uint8_t ch;

	if (c == NULL) {
		return -1;
	}
	if (ring_buf_get(&c->rx_rb, &ch, 1) == 1) {
		return (int)ch;
	}
	if (!atomic_get(&c->open)) {
		return -1;
	}
	(void)k_sem_take(&c->rx_sem, timeout);
	if (ring_buf_get(&c->rx_rb, &ch, 1) == 1) {
		return (int)ch;
	}
	return atomic_get(&c->open) ? -EAGAIN : -1;
}

int ssh_shell_getc(const struct shell *sh)
{
	struct ssh_shell_ctx *c = ctx_for(sh);
	uint8_t ch;

	if (c == NULL) {
		return -1;   /* serial console: no SSH input ring */
	}

	while (1) {
		if (ring_buf_get(&c->rx_rb, &ch, 1) == 1) {
			return (int)ch;
		}
		if (!atomic_get(&c->open)) {
			return -1;   /* session closed */
		}
		k_sem_take(&c->rx_sem, K_FOREVER);
	}
}

/* Resolve an extension file argument to an SD path: an absolute path as is,
 * else /SD:/<arg>, or /SD:/llext/<arg> (where extensions are kept) when only
 * that exists. */
static const char *ext_resolve_path(const char *arg, char *buf, size_t buflen)
{
	struct fs_dirent ent;
	char alt[128];

	if (arg[0] == '/') {
		return arg;
	}
	(void)snprintf(buf, buflen, "/SD:/%s", arg);
	if (fs_stat(buf, &ent) != 0) {
		(void)snprintf(alt, sizeof(alt), "/SD:/llext/%s", arg);
		if (fs_stat(alt, &ent) == 0) {
			(void)snprintf(buf, buflen, "%s", alt);
		}
	}
	return buf;
}

/* ── `run` command — load & execute an llext extension ───────────────────────
 * Registered here (not shell_cmds.c) because it needs the per-session WOLFSSH*.
 * Available on the serial console too: ssh_shell_ssh() returns NULL there and
 * the loader routes the extension's stdio to the UART. */

static int cmd_run(const struct shell *sh, size_t argc, char **argv)
{
	const char *arg = (argc > 1) ? argv[1] : NULL;
	char pathbuf[128];
	const char *filename;

	if (arg != NULL) {
		filename = ext_resolve_path(arg, pathbuf, sizeof(pathbuf));
	} else {
		filename = "/SD:/PROG.LLEXT";
	}

	shell_print(sh, "Loading %s ...", filename);

	/* Mark the slot busy so a concurrent disconnect (ssh_shell_release on
	 * the connection thread) waits for this program to unwind before the
	 * session is freed. */
	ssh_shell_run_begin(sh);
	int rc = llext_run_from_sd(filename, sh);

	ssh_shell_run_end(sh);

	if (rc < 0) {
		shell_print(sh, "Load/run failed (code %d)", rc);
	} else {
		shell_print(sh, "Program exited: %d", rc);
	}
	return 0;
}

SHELL_CMD_ARG_REGISTER(run, NULL,
		       "Load & run extension [file]: /SD:/<file>, else /SD:/llext/<file>"
		       " (default /SD:/PROG.LLEXT)",
		       cmd_run, 1, 1);

/* ── `svc` command — manage resident, detached background services ───────────
 * Unlike `run` (foreground, stdio bound to this session), a service extension
 * stays loaded and runs on its own thread(s).  See llext_service_* in
 * llext_loader.c.  Registered here next to `run` as it shares that loader. */

static int cmd_svc_load(const struct shell *sh, size_t argc, char **argv)
{
	char pathbuf[128];
	const char *filename = ext_resolve_path(argv[1], pathbuf, sizeof(pathbuf));
	char name[16];

	if (argc > 2) {
		/* explicit service name */
		(void)strncpy(name, argv[2], sizeof(name) - 1);
		name[sizeof(name) - 1] = '\0';
	} else {
		/* derive from the file's basename, sans directory and extension */
		const char *slash = strrchr(filename, '/');
		const char *base = (slash != NULL) ? slash + 1 : filename;
		size_t i = 0;

		while (base[i] != '\0' && base[i] != '.' &&
		       i < sizeof(name) - 1) {
			name[i] = base[i];
			i++;
		}
		name[i] = '\0';
		if (i == 0) {
			shell_print(sh, "Cannot derive a service name from '%s'; "
				    "pass one explicitly", filename);
			return -EINVAL;
		}
	}

	shell_print(sh, "Loading service '%s' from %s ...", name, filename);

	int rc = llext_service_load(filename, name);

	if (rc != 0) {
		shell_print(sh, "Load failed (%d)%s", rc,
			    rc == -EEXIST ? " — already loaded" : "");
		return rc;
	}
	shell_print(sh, "Service '%s' running", name);
	return 0;
}

static int cmd_svc_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	int rc = llext_service_stop(argv[1]);

	if (rc == -ENOENT) {
		shell_print(sh, "No such service: %s", argv[1]);
		return rc;
	}
	if (rc != 0) {
		shell_print(sh, "Stop failed (%d)", rc);
		return rc;
	}
	shell_print(sh, "Service '%s' stopped", argv[1]);
	return 0;
}

static int cmd_svc_list(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "Loaded services:");
	llext_service_list(sh);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(svc_subcmds,
	SHELL_CMD_ARG(load, NULL,
		      "load <file.llext> [name] — load & start a service"
		      " (a bare name: /SD:/, else /SD:/llext/)",
		      cmd_svc_load, 2, 1),
	SHELL_CMD_ARG(stop, NULL,
		      "stop <name> — stop & unload a service",
		      cmd_svc_stop, 2, 0),
	SHELL_CMD_ARG(list, NULL,
		      "list loaded services",
		      cmd_svc_list, 1, 0),
	SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(svc, &svc_subcmds,
		   "Manage resident background service extensions", NULL);

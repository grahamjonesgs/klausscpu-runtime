# Plan: HTTP stack as a loadable, detachable LLEXT service

Turn the statically-linked HTTP/HTTPS file server + JSON API backend into a
runtime-loadable, detachable extension that lives on the SD card, with logging
that persists to a file you can inspect.

## Goal

Today the `ssh_shell` app (the `build_ssh` "+hh" variant, with
`CONFIG_KLAUSSCPU_HTTP_SERVER` + `CONFIG_KLAUSSCPU_HTTPS_SERVER`) compiles
`httpd.c` / `httpsd.c` / `webapi.c` / `tlscert.c` directly into the firmware and
starts them from `main()` after DHCP + SD mount
(`apps/ssh_shell/src/main.c:160-172`). They run on their own worker threads.

We want instead to:

1. Build the plain HTTP server + JSON API (`/api/*`, the `index.html` backend)
   as an ELFCLASS32 `ET_REL` LLEXT object on the SD card.
2. Load it at runtime via a shell command; it **detaches** (spawns its server
   thread and returns) and **stays resident** while the thread runs.
3. Stop/unload it cleanly on demand.
4. Capture both kernel and extension diagnostics to a **file on the SD card**.

This reuses the existing LLEXT pipeline: `ssh/llext_loader.c` (loads ET_REL,
resolves `main`, runs, unloads), `ssh/llext_exports.c` (kernel→extension symbol
table), and the `run` shell command (`ssh/ssh_shell_transport.c:301`). The
`apps/llext_demo/` app is the reference for the standalone-compile recipe.

`index.html` already lives on the SD card and is served as a plain file — it is
already "detached" data and needs no work. The hardware accessors in
`ssh/board_io.h` are all `static inline` MMIO reads, so they compile *into* the
extension and need **no** exports.

## Scope decision

httpd + webapi is tractable. httpsd drags in the entire wolfSSL TLS surface
(hundreds of symbols + a large static blob), so it is phased separately.

- **Phase 1** — plain httpd + webapi as a resident, detachable extension. Prove
  the pattern.
- **Phase 2 (optional)** — httpsd. Realistic default: keep wolfSSL + httpsd
  **kernel-resident** and make only the plain server loadable, unless the TLS
  export surface proves worth it.

Logging-to-file work is shared infrastructure and lands first (Phase 0) because
both phases and the rest of the system benefit.

---

## Phase 0 — File logging (kernel-wide + extension)

### 0a. Filesystem log backend (captures everything)

Backends are additive, so this runs alongside the existing UART backend. Add to
`apps/ssh_shell/prj.conf`:

```ini
# ── File logging (to SD card) ─────────────────────────────────────────────
CONFIG_LOG_BACKEND_FS=y
CONFIG_LOG_BACKEND_FS_DIR="/SD:/log"
CONFIG_LOG_BACKEND_FS_FILE_PREFIX="klaus_"
CONFIG_LOG_BACKEND_FS_FILE_SIZE=65536      # rotate at 64 KB
CONFIG_LOG_BACKEND_FS_FILES_LIMIT=4        # keep 4 rotated files
```

Result: kernel `LOG_INF/ERR` (net, servers when kernel-resident, etc.) is
written to `/SD:/log/klaus_0.txt`, retrievable over SFTP/HTTP or `cat` from the
shell.

**Deferred vs immediate (the main decision).** The app currently runs
`CONFIG_LOG_MODE_IMMEDIATE=y` for real-time UART. Immediate mode makes every log
line a synchronous SD write on the calling thread — slow and contention-prone on
the network/server threads. For file logging switch to **deferred** mode (a
dedicated log thread drains a buffer to SD):

```ini
# replace CONFIG_LOG_MODE_IMMEDIATE=y with deferred + a sized log thread
# CONFIG_LOG_MODE_IMMEDIATE=y          # <- remove
CONFIG_LOG_MODE_DEFERRED=y
CONFIG_LOG_PROCESS_THREAD_STACK_SIZE=4096   # was 2048; SD writes need headroom
CONFIG_LOG_BUFFER_SIZE=4096
```

Trade-off to accept or revisit: deferred mode means UART messages are no longer
strictly real-time during the boot/net storm (they were already buffered when
the shell log backend was used; direct UART + deferred is a reasonable middle
ground). If real-time UART must be preserved, the alternative is the custom
appender in 0c instead of the FS backend.

**Boot ordering.** The FS backend can only write after `/SD:` is mounted
(`mount_sd()` in `main.c`). Early-boot lines buffer or fall back to UART only;
this is expected and handled gracefully by the backend.

### 0b. `ext_log` shim — route extension logging into the same file

An extension cannot use `LOG_MODULE_REGISTER` (its
`struct log_source_const_data` never joins the kernel's iterable linker
sections, so the module is never registered with the backends). Instead register
**one shared module in the kernel on behalf of extensions** and export a thin
wrapper.

New file `ssh/ext_log.c` (compiled into the kernel, gated on `CONFIG_LLEXT`):

```c
/* ext_log.c — logging entry point exported to LLEXT extensions.
 *
 * Extensions can't own a log module (LOG_MODULE_REGISTER relies on iterable
 * linker sections the llext loader doesn't merge), so the kernel registers one
 * "ext" module on their behalf and exports ext_log(). Messages flow through the
 * normal logging backends — including the filesystem backend (see prj.conf) —
 * so extension diagnostics land in /SD:/log alongside kernel logs.
 */
#include <zephyr/logging/log.h>
#include <zephyr/llext/symbol.h>

LOG_MODULE_REGISTER(ext, LOG_LEVEL_INF);

/* level: 0=err 1=wrn 2=inf (anything else -> inf). msg is pre-formatted by the
 * extension (snprintk is already exported), so no varargs cross the boundary. */
void ext_log(int level, const char *msg)
{
	switch (level) {
	case 0:  LOG_ERR("%s", msg); break;
	case 1:  LOG_WRN("%s", msg); break;
	default: LOG_INF("%s", msg); break;
	}
}
EXPORT_SYMBOL(ext_log);
```

Register it in `ssh/CMakeLists.txt` next to `llext_exports.c`:

```cmake
zephyr_library_sources_ifdef(CONFIG_LLEXT llext_exports.c ext_log.c)
```

Extension-side compat header (only active in the llext build) so the existing
`LOG_ERR/WRN/INF(...)` call sites compile unchanged — `ext/log_compat.h`:

```c
#ifndef EXT_LOG_COMPAT_H
#define EXT_LOG_COMPAT_H
#include <zephyr/sys/cbprintf.h>   /* snprintk */

extern void ext_log(int level, const char *msg);

#define EXT_LOG(lvl, ...) do {                                  \
		char _lb[160];                                  \
		(void)snprintk(_lb, sizeof(_lb), __VA_ARGS__);  \
		ext_log((lvl), _lb);                            \
	} while (0)

/* shadow the LOG_* used by httpd.c/webapi.c */
#define LOG_MODULE_REGISTER(...)            /* no-op in an extension */
#define LOG_ERR(...) EXT_LOG(0, __VA_ARGS__)
#define LOG_WRN(...) EXT_LOG(1, __VA_ARGS__)
#define LOG_INF(...) EXT_LOG(2, __VA_ARGS__)
#define LOG_DBG(...) EXT_LOG(2, __VA_ARGS__)
#endif
```

The ext build force-includes this header (`-include ext/log_compat.h`) ahead of
`<zephyr/logging/log.h>`, so `httpd.c`/`webapi.c` keep their `LOG_*` lines and
their `LOG_MODULE_REGISTER(httpd, …)` becomes a no-op.

### 0c. Alternative (no logging-subsystem change): custom file appender

If preserving real-time immediate UART matters more than a unified log: skip the
FS backend, and since the extension already needs `fs_open`/`fs_write` exported
(Phase 1), add a ~20-line appender in the extension — open `/SD:/httpd.log` in
`svc_start`, `vsnprintf` + `fs_write` per line, periodic `fs_sync`. Dedicated
server log, zero risk to existing UART logging, no immediate/deferred decision.

**Recommendation:** do 0a (deferred) + 0b. Use 0c only if deferred mode proves
unacceptable for UART.

---

## Phase 1 — Plain httpd + webapi as a detachable extension

### 1. Resident-load mode in the loader

Generalize `ssh/llext_loader.c`. The existing `llext_run_from_sd()` is for
*transient* programs (run `main`, return, unload) — a server is the opposite.

Add a small resident-service registry and two entry points:

```c
struct ext_service {
	char            name[16];
	struct llext   *ext;
	uint8_t        *buf;          /* kept alive; freed on unload      */
	int           (*stop)(void);  /* extension's svc_stop, resolved   */
};
static struct ext_service services[CONFIG_KLAUSSCPU_MAX_EXT_SERVICES];
```

- `llext_load_service(path, name)` — load the ELF (reuse `read_file()` +
  `llext_load` from the current code), resolve `svc_start` (entry) and
  `svc_stop`, call `svc_start()`, and **do not** `llext_unload`/`k_free` on
  return. Record the slot. `svc_start` must return promptly after spawning the
  server thread (the accept loop runs on that thread — this is the "detach").
- `llext_unload_service(name)` — call the recorded `svc_stop()` (closes the
  listen socket → accept loop exits → server thread joins), then `llext_unload`
  + `k_free(buf)`, then clear the slot.

Keep `keep_symtab = true` (as today) so `svc_start`/`svc_stop` resolve by name.

### 2. Make the server thread stoppable

The accept loop at `httpd.c:546-575` loops forever and nothing can stop it
today. Add:

- A `static volatile bool httpd_run;` flag.
- `httpd_start()` (→ rename concept to `svc_start`) sets `httpd_run = true`,
  creates the thread, returns.
- `svc_stop()` clears `httpd_run`, `zsock_close(srv)` to unblock `accept`, then
  `k_thread_join(&httpd_thread, K_FOREVER)` and returns.
- The accept loop checks `httpd_run` and breaks; close `srv` on exit.

### 3. Thread stack lives in the extension — join before unload

`K_THREAD_STACK_DEFINE(httpd_stack, …)` (`httpd.c:48`) lands in the extension's
`.bss`, which lives in the llext heap. That is fine **only while resident** —
which is the point — but it is exactly why `svc_stop` MUST `k_thread_join`
before `llext_unload_service` frees the image. Freeing a running thread's stack
is the sharp edge of this whole design.

### 4. Expand the export table

Audit every external symbol referenced by `httpd.c` + `webapi.c` and add the
missing ones to `ssh/llext_exports.c` via `EXPORT_SYMBOL`. (The loader fails at
load time on the first unresolved `UND`, so expect an iterate-and-add loop for
the first few loads — run `nm`/`llvm-nm -u httpd.llext` to pre-seed the list.)

Expected additions (current table has libc only):

- **Sockets:** `zsock_socket`, `zsock_bind`, `zsock_listen`, `zsock_accept`,
  `zsock_recv`, `zsock_send`, `zsock_close`, `zsock_setsockopt`.
  (`htons` / `INADDR_ANY` are macros — no export.)
- **Filesystem:** `fs_open`, `fs_close`, `fs_read`, `fs_write`, `fs_opendir`,
  `fs_readdir`, `fs_closedir`, `fs_stat`. (`fs_file_t_init` / `fs_dir_t_init`
  are inline — confirm; export if not.)
- **Kernel / threading:** `k_thread_create`, `k_thread_name_set`,
  `k_thread_join`, `k_uptime_get` (and `k_sleep`/`k_msleep` if used).
- **Formatting / libc gaps not yet exported:** `snprintk`, `strtol`, `strtoul`,
  `strstr`, `strrchr`, `isxdigit`, `tolower`. (`board_*` are inline — none.)
- **Logging:** `ext_log` (from Phase 0b) — already exported there.

### 5. Logging in the extension

Per Phase 0b: force-include `ext/log_compat.h` in the ext build so the existing
`LOG_*` calls route through the kernel `ext` module → FS backend → `/SD:/log`.
No `LOG_*` call sites in `httpd.c`/`webapi.c` need editing.

### 6. Build as an ET_REL extension on the SD card

Mirror `apps/llext_demo/CMakeLists.txt` and the standalone-clang `-c` recipe,
but output to the **SD card** (not embedded as a `.inc`), so it is genuinely
loadable. New tree `apps/http_ext/` (or `ext/httpd/`):

- Compiles `httpd.c` + `webapi.c` + a thin `svc.c` shim exposing
  `svc_start()` / `svc_stop()` (and pulling in `httpd_serve` etc.).
- `board_io.h` compiles in directly (inline MMIO).
- Force-include `ext/log_compat.h`.
- Output `httpd.llext`, staged to the SD card image.

### 7. Shell commands

Add next to the existing `run` command in `ssh/ssh_shell_transport.c` /
`ssh/shell_cmds.c`:

- `svc load /SD:/httpd.llext` → `llext_load_service`
- `svc stop httpd`            → `llext_unload_service`
- `svc list`                  → dump the `services[]` registry

### 8. Slim the firmware

For the loadable variant, drop the static path:

- Remove the `httpd_start()` call + `#ifdef CONFIG_KLAUSSCPU_HTTP_SERVER` block
  in `apps/ssh_shell/src/main.c:160-163`.
- Drop `httpd.c`/`webapi.c` from the kernel build for that variant in
  `ssh/CMakeLists.txt`.

Recommended: keep both paths behind a Kconfig (`CONFIG_KLAUSSCPU_HTTP_SERVER`
static vs a new `CONFIG_KLAUSSCPU_HTTP_LLEXT`) so you can A/B and fall back.

---

## Phase 2 — httpsd (optional)

Same resident-load mechanism, but the extension must resolve the wolfSSL entry
points httpsd uses (wolfSSL stays compiled in the kernel; only the httpsd
*logic* moves into the extension). Export at minimum:

`wolfSSL_Init`, `wolfSSL_CTX_new`, `wolfSSL_CTX_free`,
`wolfSSLv23_server_method`, `wolfSSL_CTX_SetMinVersion`, `wolfSSL_SetIORecv`,
`wolfSSL_SetIOSend`, `wolfSSL_CTX_use_certificate_buffer`,
`wolfSSL_CTX_use_PrivateKey_buffer`, `wolfSSL_new`, `wolfSSL_free`,
`wolfSSL_read`, `wolfSSL_write`, `wolfSSL_accept`, `wolfSSL_shutdown`, plus the
`tlscert_*` helpers (or export those too).

Given the surface, the realistic default is: **plain httpd loadable, httpsd
stays kernel-resident.** Revisit only if the TLS export count is justified.

---

## Risks / watch-list

- **Unload-while-running** — never `llext_unload` until `svc_stop` has
  `k_thread_join`ed the accept thread, or you free a live stack. (See Phase 1.3.)
- **Symbol completeness** — load fails on the first unresolved `UND`; pre-seed
  the export list with `llvm-nm -u httpd.llext`, expect an iterate-and-add loop.
- **wolfSSL export surface (Phase 2)** — the main cost driver; scope before
  committing.
- **Heap sizing** — `CONFIG_LLEXT_HEAP_SIZE` (currently 256 KB) must hold the
  resident image **plus** its 8 KB server-thread stack for the whole run; bump
  if loads fail with OOM. `COMMON_LIBC_MALLOC_ARENA_SIZE` already generous.
- **Deferred logging cost** — SD writes on the log thread; size
  `LOG_PROCESS_THREAD_STACK_SIZE`/`LOG_BUFFER_SIZE` and watch for dropped
  messages under load (the FS backend drops rather than blocks when the buffer
  fills).
- **FatFs reentrancy** — log writes + SD file serving + SFTP all hit FatFs
  concurrently; `CONFIG_FS_FATFS_REENTRANT=y` is already set (keep it).

## Suggested order of work

1. Phase 0a — FS backend + deferred mode; confirm `/SD:/log/klaus_0.txt` fills.
2. Phase 0b — `ext_log.c` + `ext/log_compat.h`; export `ext_log`.
3. Phase 1.4 — export-table expansion (audit-driven).
4. Phase 1.1–1.3 — resident loader + stoppable accept loop + join-before-unload.
5. Phase 1.6 — ET_REL build of `httpd.llext` to SD.
6. Phase 1.7 — `svc load/stop/list` shell commands.
7. Phase 1.8 — firmware slimming behind a Kconfig.
8. Phase 2 — evaluate; likely leave httpsd kernel-resident.

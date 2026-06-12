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
written to `/SD:/log/klaus_0000` (the backend creates the dir and names files
PREFIX + 4-digit index, no extension), retrievable over SFTP/HTTP or `cat` from the
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

The `ext_log()` prototype + level enum live in `ssh/ext_log.h`. The extension-side
compat header `ssh/ext_log_compat.h` (only active in the llext build) shadows the
`LOG_*` macros so the existing `LOG_ERR/WRN/INF(...)` call sites compile
unchanged, formatting with `snprintk` and forwarding to `ext_log()`:

```c
#include <zephyr/sys/cbprintf.h>   /* snprintk */
#include "ext_log.h"

#define EXT_LOG_AT(_lvl, ...) do {                              \
		char _ext_lb[160];                              \
		(void)snprintk(_ext_lb, sizeof(_ext_lb), __VA_ARGS__); \
		ext_log((_lvl), _ext_lb);                       \
	} while (0)

#define LOG_MODULE_REGISTER(...)            /* no-op in an extension */
#define LOG_MODULE_DECLARE(...)
#define LOG_ERR(...) EXT_LOG_AT(EXT_LOG_ERR, __VA_ARGS__)
#define LOG_WRN(...) EXT_LOG_AT(EXT_LOG_WRN, __VA_ARGS__)
#define LOG_INF(...) EXT_LOG_AT(EXT_LOG_INF, __VA_ARGS__)
#define LOG_DBG(...) EXT_LOG_AT(EXT_LOG_INF, __VA_ARGS__)
```

**Mechanism (corrected):** this header must be included *instead of*
`<zephyr/logging/log.h>`, not force-included ahead of it — a force-include is
processed first, so the real `log.h` would re-`#define` the `LOG_*` macros and
win. So the shared source guards the include (kernel-resident build unaffected):

```c
#ifdef KLAUSSCPU_LLEXT_BUILD
#include "ext_log_compat.h"
#else
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(httpd, LOG_LEVEL_INF);
#endif
```

That two-line edit to `httpd.c`/`webapi.c` lands with the extension build in
Phase 1.6; the kernel side (`ext_log.c` + the headers) is Phase 0b and is
already built/verified (`ext_log` appears in `llext_const_symbol_area`).

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

### 1–3. **(done — built; end-to-end test deferred to 1.6)**

Generalize `ssh/llext_loader.c`. The existing `llext_run_from_sd()` is for
*transient* programs (run `main`, return, unload) — a server is the opposite.

Added a resident-service registry + API (in `ssh/llext_loader.c`, declared in
`ssh/llext_loader.h`):

```c
#define EXT_SVC_MAX 4
struct ext_service {
	char          name[16];
	struct llext *ext;
	uint8_t      *buf;          /* kept alive; freed on unload */
	int         (*stop)(void);  /* extension's svc_stop        */
};
static struct ext_service svc_tab[EXT_SVC_MAX];
static K_MUTEX_DEFINE(svc_lock);
```

- `llext_service_load(path, name)` — reuses `read_file()`, `llext_load`
  (`keep_symtab=true`), resolves GLOBAL `svc_start`/`svc_stop`, calls
  `svc_start()`, and **does not** unload/free on success. Rejects a duplicate
  name (`-EEXIST`); frees the image on any error path.
- `llext_service_stop(name)` — calls the recorded `svc_stop()` (which stops AND
  joins the worker), then `llext_unload` + `k_free`, then clears the slot.
- `llext_service_list()` — prints the table.

`svc_start` must return promptly after spawning the worker (the accept loop runs
on that thread — this is the "detach").

**Stoppable accept loop (`httpd.c`).** Added `static volatile bool httpd_run`,
`httpd_start()` sets it, and a new `httpd_stop()` clears it and
`k_thread_join`s. The accept loop was rewritten to be **poll-based**:

> `accept()` ignores `SO_RCVTIMEO` in this Zephyr — `zsock_accept_ctx` passes a
> hardcoded `K_FOREVER` to `zsock_wait_data` ([sockets.c:642-648]) — so a plain
> blocking `accept()` could never time out to re-check the flag. Closing the
> listen socket from `httpd_stop()` to unblock `accept()` is racy (double-close
> vs. the worker's own close). Instead the loop does
> `zsock_poll(&{srv,POLLIN}, 1, 1000)` and only `accept()`s when POLLIN is set;
> on timeout it re-checks `httpd_run`. So **only the worker touches the socket**
> — `httpd_stop()` just flips the flag and joins (≤ ~1 s). The inner keep-alive
> loop also checks `httpd_run`, bounding stop latency to one more request.

**Join before unload (the sharp edge).** `K_THREAD_STACK_DEFINE(httpd_stack,…)`
lands in the extension's `.bss` (llext heap) once httpd is an extension. That's
fine *while resident*, but is exactly why `svc_stop` MUST `k_thread_join` the
worker before `llext_service_stop` unloads/frees the image — `httpd_stop()` does
this join, and `llext_service_stop` only unloads after `svc_stop` returns.

### 4. Expand the export table — **(done — built & verified)**

Audited with `llvm-nm -u` against the real `httpd.c.obj` / `webapi.c.obj`. **Key
finding:** Zephyr's built-in `subsys/llext/llext_export.c` already exports a huge
set — *every* `z_impl_k_*` (incl. `k_thread_join`, needed in §3) and *every*
`z_impl_zsock_*` syscall, the `mem*`/`str*` basics, uart, net_if, log — so most
references resolve with **no** export of ours. Re-exporting them just makes
duplicate `*_sym` records. See [[llext-builtin-exports]].

Genuine gaps actually added to `ssh/llext_exports.c` (13 symbols):

- **libc gaps:** `strrchr`, `strstr`, `strtol`, `strtoul` (the existing table
  already had `strchr`/`strcmp`/`strlen`/`strncmp`/`memcpy`).
- **Zephyr fmt:** `snprintk`.
- **Filesystem:** `fs_open/close/read/write/opendir/readdir/closedir/stat`,
  guarded by `#ifdef CONFIG_FILE_SYSTEM` so the FS-less `llext_demo` app (which
  also compiles this file) still links — **verified** by building both apps.

Things the audit settled:
- `webapi_handle` / `httpd_send` are internal (defined in the same `.llext`) — no export.
- `z_impl_z_log_msg_static_create` *drops out* once `ext_log_compat.h` replaces `LOG_*` — confirms Phase 0b.
- `isxdigit`/`tolower`/`htons` etc. never appear undefined — minimal-libc inlines/macros, so no export (the plan's earlier guess was wrong).
- `board_*` are inline MMIO — no export.

Syscalls are exported by their `z_impl_` target (matching Zephyr's own samples),
but in practice we needed none — they're all in the builtin table.

### 5. Logging in the extension

Per Phase 0b: under `KLAUSSCPU_LLEXT_BUILD`, include `ssh/ext_log_compat.h`
*instead of* `<zephyr/logging/log.h>` so the existing `LOG_*` calls route
through the kernel `ext` module → FS backend → `/SD:/log`. The only edit to
`httpd.c`/`webapi.c` is the two-line guarded include around their
`#include <zephyr/logging/log.h>` + `LOG_MODULE_REGISTER` (shown in Phase 0b).

### 6. Build as an ET_REL extension — **(done — built & statically verified)**

Used Zephyr's `add_llext_target()` (not the bare-metal `-c` recipe), so the
extension is compiled against **this app's** Zephyr headers/config — its ABI
matches the kernel that loads it. Wired in `apps/ssh_shell/CMakeLists.txt`,
gated on `CONFIG_LLEXT`, `add_dependencies(app httpd_ext)`; emits
`build_ssh/httpd.llext`.

Key constraints discovered and handled:
- `CONFIG_LLEXT_TYPE_ELF_OBJECT` (this app) allows **one source file** per
  llext (`add_llext_target` FATAL_ERRORs otherwise). So `ssh/httpd_ext.c` is a
  **unity TU** that `#include`s `httpd.c` + `webapi.c` and defines the
  `svc_start`/`svc_stop` shim — keeps the originals as the single source of
  truth. `board_io.h` compiles in directly (inline MMIO).
- `add_llext_target` defines `LL_EXTENSION_BUILD`; `httpd.c` keys off it to pull
  in `ext_log_compat.h`. That include must come **after** all Zephyr headers
  (e.g. `<zephyr/net/socket.h>` re-includes `<zephyr/logging/log.h>` fresh and
  would re-define `LOG_*`); placing it last is what makes the shim win. Verified
  by symbol inspection: `ext_log` *is* referenced and
  `z_impl_z_log_msg_static_create` is *gone*.

Validated statically: `httpd.llext` is **ELF32 / ET_REL / machine 0x4b43**;
`svc_start`/`svc_stop` are defined GLOBAL; and **all 33 undefined symbols are in
the kernel's 227-entry export table** (set difference empty) — so it resolves at
load. Two `*/`-in-comment self-inflicts fixed along the way.

Output is `build_ssh/httpd.llext` — copy it to the SD card (`/SD:/httpd.llext`),
e.g. via SFTP or HTTP PUT, to load it.

**Multiple-.bss fix (found on first HW load).** The loader rejects >1 SHT_NOBITS
section ("Multiple SHT_NOBITS sections are not supported", -134). The inherited
`-fdata-sections` gives each global its own `.bss.<name>`, so the build sets
`LLEXT_APPEND_FLAGS = -fno-data-sections` before `add_llext_target` to coalesce
them into one `.bss` (verify: `readelf -S | grep -c NOBITS` == 1). The
`K_THREAD_STACK_DEFINE` stack was *not* the issue — it's a single PROGBITS
`.noinit` section. Only the `.llext` changes for this fix; no firmware reflash.

### 7. Shell commands — **(done — built)**

Added a `svc` command group in `ssh/ssh_shell_transport.c` (next to `run`, which
shares the loader), wired to the Phase 1.1 API:

- `svc load <file.llext> [name]` → `llext_service_load` (name defaults to the
  file's basename sans extension, e.g. `/SD:/httpd.llext` → `httpd`)
- `svc stop <name>`              → `llext_service_stop`
- `svc list`                     → `llext_service_list(sh)`

`llext_service_list` was changed to take a `const struct shell *` so its output
reaches the SSH session (printk would only hit the UART).

### 8. Slim the firmware — **(done — chosen: manual `svc load` only)**

Decision (2026-06-11): the HTTP server comes up **only** via `svc load` — no
boot auto-start. Set `CONFIG_KLAUSSCPU_HTTP_SERVER=n` in
`apps/ssh_shell/prj.conf`, which frees `:80` (main.c's start block is already
`#ifdef`-guarded, so no code change). Note `httpd.c`/`webapi.c` REMAIN compiled
into the kernel because the HTTPS server (`:443`, still `=y`) shares
`httpd_serve`; only the plain `:80` listener thread is gone. The Kconfig option
is kept (not deleted) so the static server can be re-enabled if ever needed.

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

1. Phase 0a — FS backend + deferred mode; confirm `/SD:/log/klaus_0000` fills.
   **(done — built & confirmed on hardware)**
2. Phase 0b — `ssh/ext_log.c` + `ssh/ext_log.h` + `ssh/ext_log_compat.h`;
   export `ext_log`. **(done — built & symbol verified)**
3. Phase 1.4 — export-table expansion (audit-driven).
   **(done — built & verified; only fs_* + libc gaps needed, rest are Zephyr builtins)**
4. Phase 1.1–1.3 — resident loader + stoppable accept loop + join-before-unload.
   **(done — built; poll-based loop since accept ignores SO_RCVTIMEO)**
5. Phase 1.6 — ET_REL build of `httpd.llext` to SD.
   **(done — built via add_llext_target; statically verified resolvable)**
6. Phase 1.7 — `svc load/stop/list` shell commands. **(done — built)**
7. Phase 1.8 — firmware slimming. **(done — CONFIG_KLAUSSCPU_HTTP_SERVER=n,
   manual `svc load` only)**
8. Phase 2 — evaluate; likely leave httpsd kernel-resident.

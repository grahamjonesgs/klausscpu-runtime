/* python_cmd.c — the `python` shell command: MicroPython inside ssh_shell.
 *
 *   python             interactive REPL on this session (Ctrl-D exits)
 *   python file [args] run a script; paths resolve like the shell's (cwd)
 *
 * The interpreter runs on the session's shell thread, like `run`.  MicroPython
 * state is global, so one session at a time uses it.  ssh_shell already did
 * DHCP, NTP and the SD mount; Python sees the SD card at / (the fstab entry
 * /SD:, through ports/zephyr/zephyr_filesystem.c) and starts in the shell's
 * working directory.
 *
 * I/O: output is raw bytes to the session (ssh_shell_write; MicroPython
 * sends its own \r\n), input comes from the session's RX ring, and the
 * transport's RX hook turns Ctrl-C into a KeyboardInterrupt while code runs
 * and stops the program when the session closes.  On the serial console the
 * output goes to the UART and there is no input (a script runs; the REPL
 * exits at once).
 */
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/shell/shell.h>
#include <zephyr/random/random.h>

#include "py/compile.h"
#include "py/gc.h"
#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/runtime.h"
#include "py/cstack.h"
#include "py/stream.h"
#include "shared/runtime/gchelper.h"
#include "shared/runtime/interrupt_char.h"
#include "shared/runtime/pyexec.h"
#include "shared/readline/readline.h"
#include "extmod/vfs.h"

#include "ssh_shell_transport.h"
#include "shell_paths.h"

#define SD_PREFIX     "/SD:"

/* ── The session Python is attached to ──────────────────────────────────── */

static const struct shell *py_sh;	/* NULL: serial console      */
static volatile bool py_closed;		/* the SSH session went away  */
/* One interpreter at a time.  Not a k_mutex: those are recursive, and
 * zephyr.shell_exec('python') would re-enter on the same thread and
 * reinitialise the running interpreter's heap. */
static atomic_t py_busy;

/* The thread running Python, for socket_wrap.c; NULL when none. */
k_tid_t klausscpu_py_thread;

/* The GC heap: static rather than k_malloc'd, so it is always there and
 * the system heap (wolfSSL, net, FatFs) can't fragment it. */
static uint8_t py_heap[CONFIG_KLAUSSCPU_PYTHON_HEAP_SIZE] __aligned(16);

static const struct device *const console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

/* Connection thread: sees each received byte before the RX ring. */
static bool py_rx_hook(int c)
{
	if (c < 0) {				/* session closed */
		py_closed = true;
		mp_sched_keyboard_interrupt();
		mp_hal_signal_event();
		return true;
	}
	if (c == mp_interrupt_char) {		/* -1 while reading a line */
		mp_sched_keyboard_interrupt();
		mp_hal_signal_event();
		return true;
	}
	return false;
}

/* ── HAL: stdio and waits ───────────────────────────────────────────────── */

mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len)
{
	if (py_sh != NULL) {
		if (!py_closed) {
			(void)ssh_shell_write(py_sh, str, len);
		}
	} else {
		for (size_t i = 0; i < len; i++) {
			uart_poll_out(console, (unsigned char)str[i]);
		}
	}
	return len;
}

/* pyexec sets the interrupt character only while code runs; the REPL's line
 * editor (and raw/paste input) reads with it at -1, outside any nlr frame,
 * where raising an exception would be fatal. */
static bool py_executing(void)
{
	return mp_interrupt_char != -1;
}

int mp_hal_stdin_rx_chr(void)
{
	for (;;) {
		if (py_sh == NULL || py_closed) {
			if (py_executing()) {
				/* input() / sys.stdin in running code: end it with the
				 * KeyboardInterrupt the close scheduled, else EOFError. */
				mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS);
				mp_raise_type(&mp_type_EOFError);
			}
			return CHAR_CTRL_D;	/* line editor: leave the REPL */
		}
		int c = ssh_shell_getc_wait(py_sh, K_FOREVER);

		if (c >= 0) {
			return c;
		}
		if (c == -1) {
			py_closed = true;
			continue;
		}
		/* Woken without a byte (e.g. Ctrl-C taken by the hook). */
		if (py_executing()) {
			mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS);
		}
	}
}

uintptr_t mp_hal_stdio_poll(uintptr_t poll_flags)
{
	/* Readability of the RX ring isn't exposed; report stdin readable (as
	 * ports/zephyr does with its console subsystem). */
	return poll_flags & (MP_STREAM_POLL_RD | MP_STREAM_POLL_WR);
}

static K_SEM_DEFINE(event_sem, 0, 1);

void mp_hal_signal_event(void)
{
	k_sem_give(&event_sem);
}

void mp_hal_wait_event(bool exit_on_event, struct k_sem *sem, uint32_t timeout_ms)
{
	uint32_t t0 = k_uptime_get_32();

	for (;;) {
		mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS);

		struct k_poll_event ev[2] = {
			K_POLL_EVENT_INITIALIZER(K_POLL_TYPE_SEM_AVAILABLE,
						 K_POLL_MODE_NOTIFY_ONLY, &event_sem),
		};
		int n = 1;

		if (sem != NULL) {
			k_poll_event_init(&ev[1], K_POLL_TYPE_SEM_AVAILABLE,
					  K_POLL_MODE_NOTIFY_ONLY, sem);
			n = 2;
		}

		uint32_t dt = k_uptime_get_32() - t0;
		k_timeout_t wait = (timeout_ms == (uint32_t)-1) ? K_FOREVER
			: K_MSEC(timeout_ms > dt ? timeout_ms - dt : 0);

		(void)k_poll(ev, n, wait);
		if (ev[0].state == K_POLL_STATE_SEM_AVAILABLE) {
			(void)k_sem_take(&event_sem, K_NO_WAIT);
			if (exit_on_event) {
				return;
			}
		} else if (n == 2 && ev[1].state == K_POLL_STATE_SEM_AVAILABLE) {
			return;
		}
		if (timeout_ms != (uint32_t)-1 && k_uptime_get_32() - t0 >= timeout_ms) {
			return;
		}
	}
}

/* ── Runtime hooks ──────────────────────────────────────────────────────── */

uint32_t klausscpu_random_seed(void)
{
	return sys_rand32_get();
}

void gc_collect(void)
{
	gc_collect_start();
	gc_helper_collect_regs_and_stack();
	gc_collect_end();
}

void nlr_jump_fail(void *val)
{
	printk("python: uncaught NLR %p\n", val);
	k_panic();
	for (;;) {
	}
}

const char klausscpu_help_text[] =
	"MicroPython in ssh_shell on KlaussCPU.\r\n"
	"\r\n"
	"The SD card is the root of the filesystem: open('notes.txt'),\r\n"
	"os.listdir(), import mymodule (from the current directory or /lib).\r\n"
	"Run a script from the shell with `python file.py [args]`.\r\n"
	"\r\n"
	"Modules include: os, time, socket, select, json, re, struct, hashlib,\r\n"
	"binascii, random, math, cmath, collections, machine (mem8/16/32),\r\n"
	"zephyr.  help('modules') lists them all.\r\n"
	"\r\n"
	"Control keys: Ctrl-C interrupts, Ctrl-D exits (on an empty line),\r\n"
	"Ctrl-E paste mode.\r\n";

/* ── Running ────────────────────────────────────────────────────────────── */

/* Compile and run src; returns false (traceback printed) on an exception. */
static bool exec_str(const char *src)
{
	nlr_buf_t nlr;

	if (nlr_push(&nlr) == 0) {
		mp_lexer_t *lex = mp_lexer_new_from_str_len(MP_QSTR__lt_stdin_gt_, src,
							    strlen(src), 0);
		qstr name = lex->source_name;
		mp_parse_tree_t pt = mp_parse(lex, MP_PARSE_FILE_INPUT);
		mp_obj_t fun = mp_compile(&pt, name, false);

		mp_call_function_0(fun);
		nlr_pop();
		return true;
	}
	mp_obj_print_exception(&mp_plat_print, MP_OBJ_FROM_PTR(nlr.ret_val));
	return false;
}

/* "/SD:/a/b" -> "/a/b", "/SD:" -> "/"; other paths unchanged. */
static const char *vfs_path(const char *zephyr_path)
{
	size_t n = strlen(SD_PREFIX);

	if (strncmp(zephyr_path, SD_PREFIX, n) == 0) {
		return zephyr_path[n] != '\0' ? zephyr_path + n : "/";
	}
	return zephyr_path;
}

/* One interpreter lifetime: init, run, deinit.  Returns true when it ended
 * with a soft reset (Ctrl-D in the raw REPL), after which the caller starts
 * a fresh one, as a MicroPython board does; tools such as run-tests.py and
 * mpremote rely on that.  Ctrl-D in the normal REPL leaves `python`. */
static bool py_lifetime(const struct shell *sh, size_t argc, char **argv,
			uint8_t *heap, size_t heap_size, int *status)
{
	bool soft_reset = false;
	char path[SHELL_PATH_MAX];

	gc_init(heap, heap + heap_size);
	mp_init();
	readline_init0();		/* no history from a previous session */
	mp_obj_list_append(mp_sys_path, MP_OBJ_NEW_QSTR(MP_QSTR__slash_lib));

	/* Input hook only now: mp_init() clears pending exceptions, which would
	 * lose a close that came earlier. */
	if (py_sh != NULL) {
		ssh_shell_set_rx_hook(sh, py_rx_hook);
		if (py_closed) {
			mp_sched_keyboard_interrupt();
		}
	}

	/* SD card at /, starting in the shell's cwd (through the C API, so any
	 * directory name works). */
	if (!exec_str("import vfs, zephyr\n"
		      "vfs.mount(zephyr.FileSystem('" SD_PREFIX "'), '/')\n")) {
		*status = -EIO;
		goto out;
	}
	nlr_buf_t nlr;

	if (nlr_push(&nlr) == 0) {
		mp_vfs_chdir(mp_obj_new_str_from_cstr(vfs_path(shell_cwd(sh))));
		nlr_pop();
	} else {
		mp_obj_print_exception(&mp_plat_print, MP_OBJ_FROM_PTR(nlr.ret_val));
	}

	if (argc > 1) {
		shell_path_resolve(sh, argv[1], path, sizeof(path));
		for (size_t i = 1; i < argc; i++) {
			const char *a = (i == 1) ? vfs_path(path) : argv[i];

			mp_obj_list_append(mp_sys_argv, mp_obj_new_str_from_cstr(a));
		}
		pyexec_file(vfs_path(path));
	} else {
		mp_obj_list_append(mp_sys_argv, MP_OBJ_NEW_QSTR(MP_QSTR_));
		for (;;) {
			bool raw = (pyexec_mode_kind == PYEXEC_MODE_RAW_REPL);
			int r = raw ? pyexec_raw_repl() : pyexec_friendly_repl();

			if (py_closed) {
				break;
			}
			if (r & PYEXEC_FORCED_EXIT) {
				soft_reset = raw;	/* raw: reset; friendly: leave */
				break;
			}
			if (r != 0) {
				break;
			}
		}
	}

out:
	if (py_sh != NULL) {
		ssh_shell_set_rx_hook(sh, NULL);
	}
	gc_sweep_all();
	mp_deinit();
	return soft_reset;
}

static int run_python(const struct shell *sh, size_t argc, char **argv,
		      uint8_t *heap, size_t heap_size)
{
	int status = 0;

	/* Stack: from here down to the bottom of this (shell) thread's stack,
	 * less MICROPY_STACK_CHECK_MARGIN for the C code Python calls into
	 * (print -> wolfSSH -> crypto -> sockets).  This frame is also the top
	 * of the GC's stack scan. */
	volatile char top;
	struct k_thread *self = k_current_get();
	size_t avail = (uintptr_t)&top - self->stack_info.start;

	if (avail <= 2 * MICROPY_STACK_CHECK_MARGIN) {
		return -ENOMEM;
	}
	mp_cstack_init_with_top((void *)&top, avail);

	while (py_lifetime(sh, argc, argv, heap, heap_size, &status)) {
		mp_hal_stdout_tx_strn("MPY: soft reboot\r\n", 18);
	}
	return status;
}

static int cmd_python(const struct shell *sh, size_t argc, char **argv)
{
	if (!atomic_cas(&py_busy, 0, 1)) {
		shell_error(sh, "python is in use (another session, or nested)");
		return -EBUSY;
	}

	bool ssh = (ssh_shell_ssh(sh) != NULL);

	py_sh = ssh ? sh : NULL;
	py_closed = false;
	pyexec_mode_kind = PYEXEC_MODE_FRIENDLY_REPL;
	if (ssh) {
		ssh_shell_run_begin(sh);
	}

	klausscpu_py_thread = k_current_get();
	int rc = run_python(sh, argc, argv, py_heap, sizeof(py_heap));

	klausscpu_py_thread = NULL;

	if (rc == -ENOMEM) {
		shell_error(sh, "python: not enough stack on this thread");
	}
	if (ssh) {
		ssh_shell_run_end(sh);
	}
	py_sh = NULL;
	atomic_set(&py_busy, 0);
	return rc;
}

SHELL_CMD_ARG_REGISTER(python, NULL,
		       "MicroPython: `python` for a REPL, `python file.py [args]` to run a script",
		       cmd_python, 1, 10);

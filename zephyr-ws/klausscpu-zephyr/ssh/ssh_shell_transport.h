/* ssh_shell_transport.h — Zephyr shell backend over a wolfSSH session.
 *
 * Each SSH connection slot owns one statically-defined Zephyr shell instance.
 * Binding a live WOLFSSH* to a slot turns that shell on; all commands
 * registered with SHELL_CMD_REGISTER (see shell_cmds.c) then work over SSH
 * exactly as they do on the serial console — one command set, no duplication.
 *
 * Threading model (see ssh_shell_transport.c for the rationale):
 *   - The per-connection thread (in ssh_server.c) is the SOLE reader of the
 *     socket.  It pushes decrypted bytes into the slot's RX ring via
 *     ssh_shell_feed() and signals the shell core.
 *   - The shell core runs on its own thread (created by SHELL_DEFINE) and
 *     writes through transport write() / ssh_shell_write(); a `run`
 *     extension writes through ssh_shell_send().  The reader's
 *     wolfSSH_stream_read() also sends (window adjusts), so all of them
 *     hold the slot's io_lock (ssh_shell_io_lock) around wolfSSH calls.
 *   - A running `run` extension consumes input through ssh_shell_getc(), which
 *     drains the same RX ring, so input is never read from two threads at once.
 */

#ifndef SSH_SHELL_TRANSPORT_H
#define SSH_SHELL_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/shell/shell.h>
/* Only the type name is needed here (as wolfssh/ssh.h declares it), so
 * code without the wolfSSH include paths can use this header (the `python`
 * command's MicroPython build). */
typedef struct WOLFSSH WOLFSSH;

/* Initialise (shell_init + shell_stop) every per-slot shell instance once.
 * Call from ssh_server_start() before the listener thread is created. */
void ssh_shell_transport_init(void);

/* Bind a freshly-accepted session to slot's shell and start it.  Returns the
 * shell instance for that slot (for diagnostics; not normally needed). */
const struct shell *ssh_shell_claim(int slot, WOLFSSH *ssh);

/* Stop slot's shell and unbind the session (call on disconnect).
 *
 * Signals EOF to a running `run` extension and waits briefly for it to unwind.
 * Returns true if the session is now idle and the caller may safely free the
 * WOLFSSH* / close the socket; false if an extension ignored EOF and is still
 * executing — in that case the caller MUST NOT free the session or reuse the
 * slot (doing so would be a use-after-free on the shell thread). */
bool ssh_shell_release(int slot);

/* Push received bytes into slot's RX ring and wake the shell / any getc().
 * Called only from the per-connection (reader) thread. */
void ssh_shell_feed(int slot, const uint8_t *data, size_t len);

/* Map a shell instance back to its WOLFSSH* (NULL if `sh` is not an SSH shell,
 * e.g. the serial console).  Used by the `run` command. */
WOLFSSH *ssh_shell_ssh(const struct shell *sh);

/* Blocking single-byte read from `sh`'s RX ring; -1 on EOF/close, or if `sh`
 * is not an SSH shell.  Used by the llext input router during `run`. */
int ssh_shell_getc(const struct shell *sh);

/* As ssh_shell_getc, but returns -EAGAIN when woken without a byte (the RX
 * hook consumed it, e.g. Ctrl-C for Python) or when the timeout expires. */
int ssh_shell_getc_wait(const struct shell *sh, k_timeout_t timeout);

/* Input hook for a program running on a session (the `python` command):
 * called on the connection thread with each received byte (return true to
 * consume it, false to queue it for ssh_shell_getc), and with -1 when the
 * session closes.  Cleared when a session is claimed. */
typedef bool (*ssh_shell_rx_hook_t)(int c);
void ssh_shell_set_rx_hook(const struct shell *sh, ssh_shell_rx_hook_t hook);

/* Raw output to the session (no LF->CRLF), serialised with the shell's own
 * output.  Sends all of it, waiting for the peer's window; returns len, or
 * -EIO / -ETIMEDOUT (10 s without progress).  Dropped once the session
 * closes.  -ENOTSUP on a non-SSH shell. */
int ssh_shell_write(const struct shell *sh, const void *data, size_t len);

/* As ssh_shell_write, for code that has the session rather than the shell (a
 * `run` extension's stdout).  Output for a session that is gone is dropped. */
int ssh_shell_send(WOLFSSH *ssh, const void *data, size_t len);

/* The lock that serialises every wolfSSH call on slot's session.  The
 * connection thread holds it around wolfSSH_stream_read() (which may send a
 * window adjust or a keepalive reply) and drops it only while its socket
 * recv callback waits for data. */
struct k_mutex *ssh_shell_io_lock(int slot);

/* Bracket a program that runs on the session's shell thread, so that
 * ssh_shell_release() waits (up to 2 s) for it to unwind. */
void ssh_shell_run_begin(const struct shell *sh);
void ssh_shell_run_end(const struct shell *sh);

#endif /* SSH_SHELL_TRANSPORT_H */

/* socket_wrap.c — make Python's blocking socket calls interruptible.
 *
 * ports/zephyr/modsocket.c calls zsock_accept/recv/recvfrom/send in
 * blocking mode and has no timeouts or poll support, so `s.accept()` or
 * `s.recv()` could not be broken by Ctrl-C or by the SSH session closing:
 * the interpreter (and the session slot) stayed busy until the peer acted.
 *
 * The link wraps the implementations (-Wl,--wrap=z_impl_zsock_*, see
 * CMakeLists.txt).  For the thread running Python, on a blocking socket,
 * the wrapper first waits for readiness with zsock_poll() in 100 ms slices
 * and handles pending Python events between them (a Ctrl-C or close raises
 * KeyboardInterrupt from here, inside the running code's nlr frame).  Every
 * other caller -- ssh_shell's own servers -- goes straight through.
 */
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_context.h>
#include <zephyr/sys/fdtable.h>

#include "py/runtime.h"

#define SLICE_MS 100

extern k_tid_t klausscpu_py_thread;	/* python_cmd.c: the thread running Python */

int __real_z_impl_zsock_accept(int sock, struct net_sockaddr *addr, net_socklen_t *addrlen);
ssize_t __real_z_impl_zsock_recvfrom(int sock, void *buf, size_t max_len, int flags,
				     struct net_sockaddr *src_addr, net_socklen_t *addrlen);
ssize_t __real_z_impl_zsock_sendto(int sock, const void *buf, size_t len, int flags,
				   const struct net_sockaddr *dest_addr, net_socklen_t addrlen);

/* With TLS sockets (or offloaded ones) a stream socket's object is not a
 * net_context; stream_state() would have to tell them apart. */
#if defined(CONFIG_NET_SOCKETS_SOCKOPT_TLS) || defined(CONFIG_NET_SOCKETS_OFFLOAD)
#error "socket_wrap.c: stream_state() assumes every stream socket is a net_context"
#endif

/* A stream socket's connection state, or -1 for any other socket. */
static int stream_state(int sock)
{
	int type = 0;
	net_socklen_t len = sizeof(type);

	if (zsock_getsockopt(sock, ZSOCK_SOL_SOCKET, ZSOCK_SO_TYPE, &type, &len) < 0 ||
	    type != NET_SOCK_STREAM) {
		return -1;
	}
	struct net_context *ctx = zsock_get_context_object(sock);

	return ctx != NULL ? (int)net_context_get_state(ctx) : -1;
}

/* Wait until sock is ready for `events`, staying responsive to Ctrl-C.  Returns
 * at once when the caller isn't Python, the socket is non-blocking, poll
 * reports anything (ready, hang-up, error), or a stream socket is not in the
 * state the call needs (`want`: listening for accept, connected otherwise),
 * since poll would never report it ready: the real call then does the work
 * and reports any error itself (EINVAL, ENOTCONN). */
static void py_wait(int sock, short events, int flags, enum net_context_state want)
{
	if (klausscpu_py_thread == NULL || k_current_get() != klausscpu_py_thread ||
	    (flags & ZSOCK_MSG_DONTWAIT) != 0) {
		return;
	}
	int fl = zsock_fcntl(sock, ZVFS_F_GETFL, 0);

	if (fl < 0 || (fl & ZVFS_O_NONBLOCK) != 0) {
		return;
	}
	int st = stream_state(sock);

	if (st >= 0 && st != (int)want) {
		return;
	}
	for (;;) {
		struct zsock_pollfd p = { .fd = sock, .events = events };

		if (zsock_poll(&p, 1, SLICE_MS) != 0) {
			return;
		}
		mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS);
	}
}

int __wrap_z_impl_zsock_accept(int sock, struct net_sockaddr *addr, net_socklen_t *addrlen)
{
	py_wait(sock, ZSOCK_POLLIN, 0, NET_CONTEXT_LISTENING);
	return __real_z_impl_zsock_accept(sock, addr, addrlen);
}

ssize_t __wrap_z_impl_zsock_recvfrom(int sock, void *buf, size_t max_len, int flags,
				     struct net_sockaddr *src_addr, net_socklen_t *addrlen)
{
	py_wait(sock, ZSOCK_POLLIN, flags, NET_CONTEXT_CONNECTED);
	return __real_z_impl_zsock_recvfrom(sock, buf, max_len, flags, src_addr, addrlen);
}

ssize_t __wrap_z_impl_zsock_sendto(int sock, const void *buf, size_t len, int flags,
				   const struct net_sockaddr *dest_addr, net_socklen_t addrlen)
{
	py_wait(sock, ZSOCK_POLLOUT, flags, NET_CONTEXT_CONNECTED);
	return __real_z_impl_zsock_sendto(sock, buf, len, flags, dest_addr, addrlen);
}

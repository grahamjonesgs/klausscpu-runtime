/* ssh_server.h — SSH server for KlaussCPU Zephyr. */
#ifndef SSH_SERVER_H
#define SSH_SERVER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SSH_PORT        22
/* Concurrent SSH sessions.  Each costs a 16 KB connection-thread stack
 * (CONN_STACK_SIZE) + a per-session shell instance (CONFIG_SHELL_STACK_SIZE,
 * 40 KB) — ~56 KB each, negligible against the board's 126 MB.  The thread
 * arrays, shell instances (LISTIFY in ssh_shell_transport.c) and the listen
 * backlog all scale off this.  Net/fd limits in prj.conf (NET_MAX_CONN=20,
 * *_MAX_FDS=28) have headroom for these plus HTTPS/DNS. */
#define SSH_MAX_CONNS   5
#define SSH_STACK_SIZE  8192
#define SSH_PRIO        5

typedef int (*ssh_auth_cb_t)(const char *user, const char *password,
			     const uint8_t *pubkey, size_t pubkey_len);

int ssh_server_start(void);
void ssh_server_set_auth_cb(ssh_auth_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* SSH_SERVER_H */

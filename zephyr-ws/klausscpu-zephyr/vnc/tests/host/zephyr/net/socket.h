#pragma once
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

struct zsock_pollfd { int fd; short events; short revents; };
#define ZSOCK_POLLIN 1

int zsock_socket(int fam, int type, int proto);
int zsock_bind(int s, const struct sockaddr *a, socklen_t l);
int zsock_listen(int s, int backlog);
int zsock_accept(int s, struct sockaddr *a, socklen_t *l);
int zsock_close(int s);
ssize_t zsock_send(int s, const void *buf, size_t n, int flags);
ssize_t zsock_recv(int s, void *buf, size_t n, int flags);
int zsock_poll(struct zsock_pollfd *fds, int n, int timeout);

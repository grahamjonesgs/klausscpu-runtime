// SPDX-License-Identifier: GPL-2.0
/* A hosted (musl) smoke test for the KlaussCPU Linux port. */
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <sched.h>
#include <unistd.h>

static volatile sig_atomic_t got;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static long locked_count;
static int atomic_count;

static void *worker(void *arg)
{
	int i;

	for (i = 0; i < 2000; i++) {
		pthread_mutex_lock(&mu);
		locked_count++;
		pthread_mutex_unlock(&mu);
		__atomic_fetch_add(&atomic_count, 1, __ATOMIC_SEQ_CST);
		if ((i & 255) == 0)
			sched_yield();
	}
	return arg;
}
static jmp_buf jb;

static void on_usr1(int sig, siginfo_t *si, void *uc)
{
	got = sig;
}

int main(int argc, char **argv)
{
	struct sigaction sa;
	struct timespec ts;
	char *p;
	pid_t pid;
	int status, fails = 0;

	printf("musltest: hello from musl, argc=%d argv[0]=%s pid=%d\n", argc, argv[0], getpid());

	p = malloc(100000);
	memset(p, 'x', 100000);
	p[99999] = 0;
	printf("musltest: malloc+memset ok, strlen=%zu\n", strlen(p));
	free(p);

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = on_usr1;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGUSR1, &sa, NULL);
	kill(getpid(), SIGUSR1);
	printf("musltest: signal handler ran: %s\n", got == SIGUSR1 ? "yes" : "NO");
	fails += got != SIGUSR1;

	if (setjmp(jb) == 0)
		longjmp(jb, 42);
	printf("musltest: setjmp/longjmp ok\n");

	clock_gettime(CLOCK_MONOTONIC, &ts);
	printf("musltest: clock_gettime %lld.%09ld\n", (long long)ts.tv_sec, ts.tv_nsec);

	pid = fork();
	if (pid == 0) {
		printf("musltest: child %d running\n", getpid());
		exit(5);
	}
	waitpid(pid, &status, 0);
	printf("musltest: child exited with %d\n", WEXITSTATUS(status));
	fails += WEXITSTATUS(status) != 5;

	/* TCP over loopback: the IPv4 stack on this arch (checksums, sockets). */
	{
		struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(5555) };
		struct ifreq ifr = { 0 };
		char buf[64] = { 0 };
		int s, c, one = 1, ok = 0;

		s = socket(AF_INET, SOCK_DGRAM, 0);
		strcpy(ifr.ifr_name, "lo");
		ioctl(s, SIOCGIFFLAGS, &ifr);
		ifr.ifr_flags |= IFF_UP;
		ioctl(s, SIOCSIFFLAGS, &ifr);	/* bring lo up (no rcS here) */
		close(s);
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		s = socket(AF_INET, SOCK_STREAM, 0);
		setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		if (bind(s, (void *)&a, sizeof(a)) == 0 && listen(s, 1) == 0) {
			pid = fork();
			if (pid == 0) {
				c = socket(AF_INET, SOCK_STREAM, 0);
				if (connect(c, (void *)&a, sizeof(a)) == 0) {
					write(c, "ping over tcp", 13);
					read(c, buf, sizeof(buf) - 1);
				}
				_exit(strcmp(buf, "PING OVER TCP") != 0);
			}
			c = accept(s, NULL, NULL);
			if (c >= 0) {
				int n = read(c, buf, sizeof(buf) - 1), i;

				for (i = 0; i < n; i++)
					buf[i] = buf[i] >= 'a' && buf[i] <= 'z' ? buf[i] - 32 : buf[i];
				write(c, buf, n);
				close(c);
			}
			waitpid(pid, &status, 0);
			ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
		}
		close(s);
		printf("musltest: tcp loopback echo %s\n", ok ? "ok" : "FAILED");
		fails += !ok;
	}

	/* Threads: clone, TP, futex wait/wake under a contended mutex. */
	{
		pthread_t t[4];
		void *r;
		int i, bad = 0;

		for (i = 0; i < 4; i++)
			pthread_create(&t[i], NULL, worker, (void *)(long)i);
		for (i = 0; i < 4; i++) {
			pthread_join(t[i], &r);
			bad += r != (void *)(long)i;
		}
		printf("musltest: 4 threads: mutex count %ld, atomic count %d\n",
		       locked_count, atomic_count);
		fails += bad || locked_count != 8000 || atomic_count != 8000;
	}

	/* 300 short-lived address spaces: ASID generations roll over. */
	{
		static volatile long cell = 7;
		int i, bad = 0;

		for (i = 0; i < 300; i++) {
			pid = fork();
			if (pid == 0) {
				cell = 1000 + i;
				_exit(cell == 1000 + i ? 0 : 1);
			}
			waitpid(pid, &status, 0);
			bad += WEXITSTATUS(status) != 0 || cell != 7;
		}
		printf("musltest: 300 forks, %d bad\n", bad);
		fails += bad != 0;
	}

	printf("musltest: %s\n", fails ? "FAIL" : "PASS");
	if (getpid() == 1)
		for (;;)
			pause();
	return fails;
}

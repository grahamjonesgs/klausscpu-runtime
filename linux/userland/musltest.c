// SPDX-License-Identifier: GPL-2.0
/* A hosted (musl) smoke test for the KlaussCPU Linux port. */
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t got;
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

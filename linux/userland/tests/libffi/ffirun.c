// SPDX-License-Identifier: GPL-2.0
/*
 * ffirun LIST: runs each program named in LIST (lines "name [VAR=value...]
 * path [args]"),
 * one at a time, with its stdout and stderr captured through a pipe, and
 * prints
 *	@@BEGIN name
 *	<everything it wrote>
 *	@@END name status=<waitpid status> exit=<code>|signal=<n>
 * so that run.sh can check the output on the host.  Each program gets
 * TIMEOUT seconds (alarm, which survives execve).  Static, from minish.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define TIMEOUT 120

static void run(char *name, char **argv)
{
	int fd[2];
	pid_t pid;
	int st;
	char buf[4096];
	ssize_t n;

	printf("@@BEGIN %s\n", name);
	fflush(stdout);
	if (pipe(fd) < 0) {
		printf("\n@@END %s pipe: %s\n", name, strerror(errno));
		return;
	}
	pid = fork();
	if (pid == 0) {
		dup2(fd[1], 1);
		dup2(fd[1], 2);
		close(fd[0]);
		close(fd[1]);
		alarm(TIMEOUT);
		while (argv[0] && strchr(argv[0], '='))
			putenv(*argv++);
		execv(argv[0], argv);
		dprintf(2, "exec %s: %s\n", argv[0], strerror(errno));
		_exit(127);
	}
	close(fd[1]);
	while ((n = read(fd[0], buf, sizeof buf)) > 0 || (n < 0 && errno == EINTR))
		if (n > 0)
			fwrite(buf, 1, n, stdout);
	close(fd[0]);
	while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
		;
	if (WIFEXITED(st))
		printf("\n@@END %s exit=%d\n", name, WEXITSTATUS(st));
	else if (WIFSIGNALED(st))
		printf("\n@@END %s signal=%d\n", name, WTERMSIG(st));
	else
		printf("\n@@END %s status=%#x\n", name, st);
	fflush(stdout);
}

int main(int argc, char **argv)
{
	char line[1024];
	FILE *f;
	int count = 0;

	if (argc != 2) {
		fprintf(stderr, "usage: ffirun LIST\n");
		return 2;
	}
	f = fopen(argv[1], "r");
	if (!f) {
		perror(argv[1]);
		return 2;
	}
	while (fgets(line, sizeof line, f)) {
		char *av[16], *name;
		int ac = 0;

		name = strtok(line, " \t\n");
		if (!name || name[0] == '#')
			continue;
		while (ac < 15 && (av[ac] = strtok(NULL, " \t\n")))
			ac++;
		av[ac] = NULL;
		if (!ac)
			continue;
		run(name, av);
		count++;
	}
	fclose(f);
	printf("@@DONE %d\n", count);
	return 0;
}

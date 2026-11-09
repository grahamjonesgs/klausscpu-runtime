// SPDX-License-Identifier: GPL-2.0
/*
 * minish: a small shell for the KlaussCPU Linux port (musl, static) while
 * busybox is not built.  Reads lines from the console (hvc0) and runs
 * built-ins or programs (fork, execve, waitpid).
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static int split(char *line, char **argv, int max)
{
	int n = 0;
	char *p = strtok(line, " \t\n");

	while (p && n < max - 1) {
		argv[n++] = p;
		p = strtok(NULL, " \t\n");
	}
	argv[n] = NULL;
	return n;
}

static void cat(const char *path)
{
	char buf[512];
	ssize_t n;
	int fd = open(path, O_RDONLY);

	if (fd < 0) {
		printf("cat: %s: %s\n", path, strerror(errno));
		return;
	}
	while ((n = read(fd, buf, sizeof(buf))) > 0)
		fwrite(buf, 1, n, stdout);
	close(fd);
}

static void ls(const char *path)
{
	DIR *d = opendir(path);
	struct dirent *e;

	if (!d) {
		printf("ls: %s: %s\n", path, strerror(errno));
		return;
	}
	while ((e = readdir(d)))
		if (e->d_name[0] != '.')
			printf("%s\n", e->d_name);
	closedir(d);
}

static void ps(void)
{
	DIR *d = opendir("/proc");
	struct dirent *e;
	char path[64], comm[64];

	if (!d) {
		printf("ps: mount proc first (mount)\n");
		return;
	}
	printf("  PID COMMAND\n");
	while ((e = readdir(d))) {
		FILE *f;

		if (e->d_name[0] < '0' || e->d_name[0] > '9')
			continue;
		snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
		f = fopen(path, "r");
		if (!f)
			continue;
		if (fgets(comm, sizeof(comm), f))
			printf("%5s %s", e->d_name, comm);
		fclose(f);
	}
	closedir(d);
}

static int run(char **argv)
{
	int status;
	pid_t pid = fork();

	if (pid < 0) {
		printf("fork: %s\n", strerror(errno));
		return -1;
	}
	if (pid == 0) {
		execve(argv[0], argv, environ);
		printf("%s: %s\n", argv[0], strerror(errno));
		_exit(127);
	}
	waitpid(pid, &status, 0);
	if (WIFSIGNALED(status))
		printf("%s: killed by signal %d\n", argv[0], WTERMSIG(status));
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int main(void)
{
	char line[256], cwd[256], *argv[32];
	struct utsname u;
	int argc, i;

	setvbuf(stdout, NULL, _IONBF, 0);
	printf("minish: KlaussCPU Linux shell (built-ins: echo cd pwd ls cat mkdir rm "
	       "uname mount ps exit; or a /path/to/program)\n");
	for (;;) {
		printf("$ ");
		if (!fgets(line, sizeof(line), stdin))
			break;
		argc = split(line, argv, 32);
		if (argc == 0)
			continue;
		if (!strcmp(argv[0], "exit")) {
			break;
		} else if (!strcmp(argv[0], "echo")) {
			for (i = 1; i < argc; i++)
				printf("%s%s", argv[i], i + 1 < argc ? " " : "");
			printf("\n");
		} else if (!strcmp(argv[0], "cd")) {
			if (chdir(argc > 1 ? argv[1] : "/"))
				printf("cd: %s\n", strerror(errno));
		} else if (!strcmp(argv[0], "pwd")) {
			printf("%s\n", getcwd(cwd, sizeof(cwd)) ? cwd : "?");
		} else if (!strcmp(argv[0], "ls")) {
			ls(argc > 1 ? argv[1] : ".");
		} else if (!strcmp(argv[0], "cat")) {
			for (i = 1; i < argc; i++)
				cat(argv[i]);
		} else if (!strcmp(argv[0], "mkdir")) {
			for (i = 1; i < argc; i++)
				if (mkdir(argv[i], 0755))
					printf("mkdir: %s: %s\n", argv[i], strerror(errno));
		} else if (!strcmp(argv[0], "rm")) {
			for (i = 1; i < argc; i++)
				if (unlink(argv[i]) && rmdir(argv[i]))
					printf("rm: %s: %s\n", argv[i], strerror(errno));
		} else if (!strcmp(argv[0], "uname")) {
			uname(&u);
			printf("%s %s %s %s\n", u.sysname, u.nodename, u.release, u.machine);
		} else if (!strcmp(argv[0], "mount")) {
			if (mount("proc", "/proc", "proc", 0, NULL))
				printf("mount: %s\n", strerror(errno));
		} else if (!strcmp(argv[0], "ps")) {
			ps();
		} else if (strchr(argv[0], '/')) {
			int rc = run(argv);

			if (rc)
				printf("[exit %d]\n", rc);
		} else {
			printf("%s: not found\n", argv[0]);
		}
	}
	printf("minish: bye\n");
	if (getpid() == 1)
		for (;;)
			pause();
	return 0;
}

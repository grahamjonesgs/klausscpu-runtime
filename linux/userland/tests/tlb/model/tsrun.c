/* tsrun LIST: lines "name [<file] [VAR=val...] argv...": run each, stdin from file if given. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
int main(int argc, char **argv)
{
	char line[1024];
	FILE *f = fopen(argv[1], "r");
	while (f && fgets(line, sizeof line, f)) {
		char *av[24], *name = strtok(line, " \t\n"), *in = NULL;
		int ac = 0, st;
		if (!name || name[0] == '#') continue;
		while (ac < 23 && (av[ac] = strtok(NULL, " \t\n"))) ac++;
		av[ac] = NULL;
		printf("@@BEGIN %s\n", name); fflush(stdout);
		pid_t pid = fork();
		if (pid == 0) {
			char **a = av;
			if (a[0] && a[0][0] == '<') { int fd = open(a[0] + 1, O_RDONLY); dup2(fd, 0); a++; }
			while (a[0] && strchr(a[0], '=')) putenv(*a++);
			execv(a[0], a);
			perror(a[0]); _exit(127);
		}
		waitpid(pid, &st, 0);
		printf("@@END %s status=%#x\n", name, st); fflush(stdout);
	}
	printf("@@ALL-DONE\n");
	return 0;
}

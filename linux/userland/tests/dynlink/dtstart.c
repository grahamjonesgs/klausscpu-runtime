/*
 * dt-start: start-up time of a static against a dynamically linked
 * program.  For each path: one warm-up run, then N fork/execve/waitpid
 * runs timed with CLOCK_MONOTONIC.
 *   dt-start N path...
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static int run(const char *path)
{
	int st;
	pid_t pid = fork();

	if (pid == 0) {
		char *av[] = { (char *)path, NULL };

		execve(path, av, environ);
		_exit(126);
	}
	if (pid < 0 || waitpid(pid, &st, 0) != pid)
		return -1;
	if (!WIFEXITED(st) || WEXITSTATUS(st))
		printf("INFO %s pid %d: %s %d\n", path, (int)pid, WIFEXITED(st) ? "exit" : "signal",
		       WIFEXITED(st) ? WEXITSTATUS(st) : WTERMSIG(st));
	return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

int main(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 200, fails = 0;

	for (int a = 2; a < argc; a++) {
		struct timespec t0, t1;
		int bad = run(argv[a]) != 0;
		double s;

		clock_gettime(CLOCK_MONOTONIC, &t0);
		for (int i = 0; i < n; i++)
			bad += run(argv[a]) != 0;
		clock_gettime(CLOCK_MONOTONIC, &t1);
		s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
		printf("TIME %s runs=%d failed=%d total=%.3f s per-run=%.3f ms\n", argv[a], n, bad, s, s * 1e3 / n);
		fails += bad;
	}
	printf("%s start-time: %d failed runs\n", fails ? "FAIL" : "PASS", fails);
	return fails != 0;
}

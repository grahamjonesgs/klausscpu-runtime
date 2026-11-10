// SPDX-License-Identifier: GPL-2.0
/*
 * perfstat [-d N] command [args]: run command with the board's performance
 * counters (/proc/klausscpu_perf, arch/klausscpu/kernel/perf.c) cleared
 * before it and read after it, and print them.  -d N divides every count
 * by N (per iteration of a benchmark loop of N iterations).  The counters
 * are global, so the result includes the kernel, other processes and idle
 * cycles: run it on an otherwise quiet system.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define PERF_FILE "/proc/klausscpu_perf"
#define MAXC 64

static char names[MAXC][32];
static unsigned long long vals[MAXC];
static int n;

static unsigned long long get(const char *name)
{
	for (int i = 0; i < n; i++)
		if (!strcmp(names[i], name))
			return vals[i];
	return 0;
}

int main(int argc, char **argv)
{
	double div = 1;
	FILE *f;
	pid_t pid;
	int st, i = 1;

	if (i + 1 < argc && !strcmp(argv[i], "-d")) {
		div = atof(argv[i + 1]);
		i += 2;
	}
	if (i >= argc || div <= 0) {
		fprintf(stderr, "usage: perfstat [-d N] command [args]\n");
		return 2;
	}
	f = fopen(PERF_FILE, "w");
	if (!f || fputs("0\n", f) < 0 || fclose(f)) {
		perror(PERF_FILE);
		return 1;
	}
	pid = fork();
	if (pid == 0) {
		execvp(argv[i], argv + i);
		perror(argv[i]);
		_exit(127);
	}
	if (pid < 0 || waitpid(pid, &st, 0) < 0) {
		perror("perfstat");
		return 1;
	}
	f = fopen(PERF_FILE, "r");
	if (!f) {
		perror(PERF_FILE);
		return 1;
	}
	while (n < MAXC && fscanf(f, "%31s %llu", names[n], &vals[n]) == 2)
		n++;
	fclose(f);

	unsigned long long cyc = get("cycles"), ins = get("instructions");
	unsigned long long busy = cyc - get("idle_cycles");

	fprintf(stderr, "perfstat: %s%s\n", argv[i], div != 1 ? " (per iteration)" : "");
	for (int k = 0; k < n; k++) {
		fprintf(stderr, "  %-16s %14.1f", names[k], vals[k] / div);
		if (busy && (!strncmp(names[k], "stall_", 6) || !strcmp(names[k], "if_miss") ||
			     !strcmp(names[k], "mem_wait") || !strcmp(names[k], "ic_fill_cycles") ||
			     !strcmp(names[k], "dc_stall_cycles") || !strcmp(names[k], "idle_cycles")))
			fprintf(stderr, "  %5.1f%% of cycles", 100.0 * vals[k] / cyc);
		fputc('\n', stderr);
	}
	if (ins)
		fprintf(stderr, "  CPI %.2f (busy cycles / instructions %.2f)\n",
			(double)cyc / ins, (double)busy / ins);
	return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

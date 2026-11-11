/* xchroot DIR PROG [ARGS...]: chroot(DIR), chdir("/"), execv(PROG). */
#include <stdio.h>
#include <unistd.h>
int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: xchroot dir prog [args]\n"); return 2; }
	if (chroot(argv[1]) || chdir("/")) { perror("xchroot"); return 1; }
	execv(argv[2], argv + 2);
	perror(argv[2]);
	return 127;
}

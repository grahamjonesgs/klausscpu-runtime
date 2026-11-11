/* libpre.so: LD_PRELOADed by dtmain's "preload" test.  Overrides libc's
 * getpid, counts the calls and forwards to the real one (RTLD_NEXT). */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <unistd.h>

int pre_getpid_count;
void *pre_real_getpid;

pid_t getpid(void)
{
	pid_t (*real)(void) = (pid_t (*)(void))pre_real_getpid;

	if (!real)
		pre_real_getpid = real = (pid_t (*)(void))dlsym(RTLD_NEXT, "getpid");
	pre_getpid_count++;
	return real ? real() : -1;
}

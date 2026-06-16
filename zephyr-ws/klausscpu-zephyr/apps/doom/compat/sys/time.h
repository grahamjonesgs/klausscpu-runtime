/* Minimal POSIX <sys/time.h> for the Doom port. */
#ifndef DOOM_COMPAT_SYS_TIME_H
#define DOOM_COMPAT_SYS_TIME_H

struct timeval {
	long tv_sec;
	long tv_usec;
};
struct timezone {
	int tz_minuteswest;
	int tz_dsttime;
};

int gettimeofday(struct timeval *tv, void *tz);

#endif

/* Minimal POSIX <sys/ioctl.h> for the Doom port (stubbed). */
#ifndef DOOM_COMPAT_SYS_IOCTL_H
#define DOOM_COMPAT_SYS_IOCTL_H
int ioctl(int fd, unsigned long request, ...);
#endif

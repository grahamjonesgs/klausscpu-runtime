/* Minimal POSIX <sys/stat.h> for the Doom port. */
#ifndef DOOM_COMPAT_SYS_STAT_H
#define DOOM_COMPAT_SYS_STAT_H
#include <sys/types.h>

struct stat {
	off_t  st_size;
	mode_t st_mode;
};

#define S_IFMT  0xF000
#define S_IFREG 0x8000
#define S_IFDIR 0x4000
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)

int stat(const char *path, struct stat *st);
int fstat(int fd, struct stat *st);
int mkdir(const char *path, mode_t mode);

#endif

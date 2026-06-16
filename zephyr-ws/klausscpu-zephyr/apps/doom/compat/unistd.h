/* Minimal POSIX <unistd.h> for the Doom port. */
#ifndef DOOM_COMPAT_UNISTD_H
#define DOOM_COMPAT_UNISTD_H
#include <stddef.h>
#include <sys/types.h>

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

int      isatty(int fd);
int      usleep(unsigned long usec);
unsigned sleep(unsigned sec);
int      access(const char *path, int mode);
int      unlink(const char *path);
int      close(int fd);
ssize_t  read(int fd, void *buf, size_t n);
ssize_t  write(int fd, const void *buf, size_t n);
off_t    lseek(int fd, off_t off, int whence);
int      getpid(void);
char    *getcwd(char *buf, size_t size);

#endif

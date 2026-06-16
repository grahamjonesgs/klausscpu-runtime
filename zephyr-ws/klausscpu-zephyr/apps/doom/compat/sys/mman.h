/* Minimal POSIX <sys/mman.h> for the Doom port (mmap WAD path unused). */
#ifndef DOOM_COMPAT_SYS_MMAN_H
#define DOOM_COMPAT_SYS_MMAN_H
#include <sys/types.h>

#define PROT_READ   1
#define PROT_WRITE  2
#define MAP_SHARED  1
#define MAP_PRIVATE 2
#define MAP_FAILED  ((void *)-1)

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
int   munmap(void *addr, size_t len);

#endif
